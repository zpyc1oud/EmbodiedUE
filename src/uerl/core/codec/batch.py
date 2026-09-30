"""Validate and map negotiated U4 binary batch layouts and payloads."""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from typing import Any

import numpy as np

from ...errors import BridgeProtocolError
from .frames import MAX_DATA_PAYLOAD_BYTES, PROTOCOL
from .json import sha256_json

_LAYOUT_KINDS = {"initial_state", "step_action", "step_result", "reset_request", "reset_result"}
_DTYPE_SIZES = {"float32": 4, "int32": 4, "uint8": 1, "uint16": 2, "uint64": 8}
_NAME_PATTERN = re.compile(r"^[a-z][a-z0-9_.]*$")


@dataclass(frozen=True, slots=True)
class Segment:
    """Describe one named binary batch segment.

    Attributes:
        name: Stable protocol field name.
        dtype: Supported little-endian scalar dtype.
        shape: Positive tensor shape including the negotiated batch dimension.
        offset: Eight-byte-aligned byte offset within the payload.
        byte_length: Exact encoded size of the segment.
    """

    name: str
    dtype: str
    shape: tuple[int, ...]
    offset: int
    byte_length: int


@dataclass(frozen=True, slots=True)
class Layout:
    """Represent a validated binary layout descriptor.

    The segment tuple is ordered as negotiated and the layout identity is
    derived from the protocol version, batch size, encoding, and raw descriptor
    order. A validated Layout may be used to map payload memory without
    re-negotiating its fields.
    """

    kind: str
    payload_length: int
    layout_id: int
    layout_hash: str
    segments: tuple[Segment, ...]

    @classmethod
    def parse(
        cls,
        descriptor: dict[str, Any],
        batch_size: int,
        protocol_version: tuple[int, int] = PROTOCOL,
    ) -> Layout:
        """Parse and validate one external Layout descriptor.

        Args:
            descriptor: External JSON layout DTO containing encoding, ordered
                segments, payload length, hash, and ID.
            batch_size: Positive Worker Slot count used in layout identity
                validation.
            protocol_version: Selected protocol major/minor included in the
                canonical layout hash.

        Returns:
            A frozen Layout with typed segments and the expected numeric layout
            identity.

        Raises:
            BridgeProtocolError: If fields, names, dtypes, shapes, alignment,
                padding, payload ranges, hash, or ID are invalid.
        """

        try:
            if not isinstance(batch_size, int) or isinstance(batch_size, bool) or batch_size < 1:
                raise BridgeProtocolError("layout batch size must be positive", code="LAYOUT_BATCH_SIZE")
            if descriptor["layout_kind"] not in _LAYOUT_KINDS:
                raise BridgeProtocolError("unknown layout kind", code="LAYOUT_KIND")
            if descriptor["byte_order"] != "little" or descriptor["alignment"] != 8:
                raise BridgeProtocolError("unsupported layout byte order or alignment", code="LAYOUT_ENCODING")
            payload_length = descriptor["payload_length"]
            if (
                not isinstance(payload_length, int)
                or isinstance(payload_length, bool)
                or not 0 <= payload_length <= MAX_DATA_PAYLOAD_BYTES
            ):
                raise BridgeProtocolError("invalid layout payload length", code="LAYOUT_PAYLOAD_LENGTH")
            raw_segments = descriptor["segments"]
            if not isinstance(raw_segments, list):
                raise BridgeProtocolError("layout segments must be an array", code="LAYOUT_SEGMENTS")
            segments_list: list[Segment] = []
            cursor = 0
            names: set[str] = set()
            for item in raw_segments:
                if not isinstance(item, dict):
                    raise BridgeProtocolError("layout segment must be an object", code="LAYOUT_SEGMENT")
                if set(item) != {"name", "dtype", "shape", "offset", "byte_length"}:
                    raise BridgeProtocolError("layout segment has unknown fields", code="LAYOUT_SEGMENT_FIELDS")
                name = item["name"]
                dtype = item["dtype"]
                shape = item["shape"]
                offset = item["offset"]
                byte_length = item["byte_length"]
                if not isinstance(name, str) or _NAME_PATTERN.fullmatch(name) is None or name in names:
                    raise BridgeProtocolError("invalid or duplicate layout segment name", code="LAYOUT_SEGMENT_NAME")
                if dtype not in _DTYPE_SIZES:
                    raise BridgeProtocolError("unsupported layout dtype", code="LAYOUT_DTYPE")
                if not isinstance(shape, list) or any(
                    not isinstance(dimension, int) or isinstance(dimension, bool) or dimension < 1
                    for dimension in shape
                ):
                    raise BridgeProtocolError(
                        "layout segment shape must contain positive integers",
                        code="LAYOUT_SHAPE",
                    )
                if not all(isinstance(value, int) and not isinstance(value, bool) for value in (offset, byte_length)):
                    raise BridgeProtocolError("layout segment offsets must be integers", code="LAYOUT_SEGMENT_RANGE")
                expected_bytes = _DTYPE_SIZES[dtype] * math.prod(shape)
                if byte_length != expected_bytes or offset < cursor or offset % 8 != 0:
                    raise BridgeProtocolError("layout segment range is invalid", code="LAYOUT_SEGMENT_RANGE")
                end = offset + byte_length
                if end > payload_length:
                    raise BridgeProtocolError("layout segment exceeds payload length", code="LAYOUT_SEGMENT_RANGE")
                names.add(name)
                segments_list.append(Segment(name, dtype, tuple(shape), offset, byte_length))
                cursor = end
            if (segments_list and segments_list[0].offset != 0) or cursor != payload_length:
                raise BridgeProtocolError("layout payload length does not match segments", code="LAYOUT_PAYLOAD_LENGTH")
            segments = tuple(segments_list)
            # Hash the raw ordered descriptors so reordering fields or changing
            # protocol/batch identity cannot silently reuse a binary layout.
            hash_input = {
                "protocol_version": {"major": protocol_version[0], "minor": protocol_version[1]},
                "layout_kind": descriptor["layout_kind"],
                "batch_size": batch_size,
                "byte_order": descriptor["byte_order"],
                "alignment": descriptor["alignment"],
                "ordered_segments": raw_segments,
            }
            expected_hash = sha256_json(hash_input)
            if descriptor["layout_hash"] != expected_hash:
                raise BridgeProtocolError("layout hash mismatch", code="LAYOUT_HASH_MISMATCH")
            expected_id = int(expected_hash[:16], 16)
            if descriptor["layout_id"] != f"{expected_id:016x}":
                raise BridgeProtocolError("layout id mismatch", code="LAYOUT_ID_MISMATCH")
            return cls(descriptor["layout_kind"], payload_length, expected_id, expected_hash, segments)
        except BridgeProtocolError:
            raise
        except (KeyError, TypeError, ValueError) as exc:
            raise BridgeProtocolError("layout descriptor is missing a required field", code="LAYOUT_FIELD") from exc

    def segment(self, name: str) -> Segment:
        """Return a named segment from this validated layout.

        Args:
            name: Exact negotiated segment name.

        Returns:
            The matching immutable Segment descriptor.

        Raises:
            StopIteration: If ``name`` is not present. Callers at the protocol
                boundary should validate required names before encoding/decoding.
        """

        return next(segment for segment in self.segments if segment.name == name)


class BatchCodec:
    """Validate one negotiated binary batch before it reaches task code.

    Validation enforces the negotiated payload length, zero padding, and finite
    float values before returning segment views to higher layers.
    """

    @staticmethod
    def map_trusted(layout: Layout, payload: bytes | bytearray) -> dict[str, memoryview]:
        """Map a payload after the Bridge boundary has validated it.

        The raw Bridge session validates payloads before exposing them to the
        typed Direct adapter. This method only creates negotiated segment views
        and intentionally does not repeat wire validation in that trusted path.
        """

        return _map_segments(layout, payload)

    @staticmethod
    def validate_and_map(layout: Layout, payload: bytes | bytearray) -> dict[str, memoryview]:
        """Check exact length, zero padding, and finite float fields.

        Args:
            layout: Previously validated negotiated layout for this payload kind.
            payload: Bytes or bytearray whose length and field encoding must
                exactly match ``layout``.

        Returns:
            A mapping from segment name to memoryview slices of ``payload``.
            The views alias the supplied buffer and are read-only only when the
            supplied buffer is read-only.

        Raises:
            BridgeProtocolError: If the length, padding, or float finiteness
                violates the negotiated batch contract.
        """

        if len(payload) != layout.payload_length:
            raise BridgeProtocolError("batch payload length mismatch", code="PAYLOAD_LENGTH")
        view = memoryview(payload)
        mapped = _map_segments(layout, payload)
        cursor = 0
        for segment in layout.segments:
            # Padding is part of the negotiated layout envelope and must not
            # carry hidden data between fields or at the payload tail.
            padding = view[cursor:segment.offset]
            if any(padding):
                raise BridgeProtocolError("binary batch padding must be zero", code="PAYLOAD_PADDING")
            segment_view = view[segment.offset : segment.offset + segment.byte_length]
            if segment.dtype == "float32" and not np.isfinite(np.frombuffer(segment_view, dtype="<f4")).all():
                raise BridgeProtocolError("binary batch contains a non-finite value", code="PAYLOAD_NON_FINITE")
            cursor = segment.offset + segment.byte_length
        if any(view[cursor:]):
            raise BridgeProtocolError("binary batch padding must be zero", code="PAYLOAD_PADDING")
        return mapped


def _map_segments(layout: Layout, payload: bytes | bytearray) -> dict[str, memoryview]:
    """Create segment views for a layout whose descriptor is already trusted."""

    view = memoryview(payload)
    return {
        segment.name: view[segment.offset : segment.offset + segment.byte_length]
        for segment in layout.segments
    }

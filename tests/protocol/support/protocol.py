"""Define the U4 SocketBridge framing, hashing, and layout test oracle."""

from __future__ import annotations

import hashlib
import json
import math
import struct
from dataclasses import dataclass
from typing import Any
from uuid import UUID

import numpy as np

from uerl import (
    ObservationShapeTable,
    ObsType,
    RobotSpec,
    RobotTopology,
    merge_robot_spec,
    robot_actuator_action_schema,
    robot_observation_schema,
)
from uerl.core.config.canonical import to_jsonable
from uerl.core.direct.types import SessionSchema
from uerl.tasks.cartpole import load_cartpole_training_config

MAGIC = 0x5545524C
PROTOCOL = (2, 0)
HEADER = struct.Struct(">IHHHHIQ16sQ")
HEADER_SIZE = 48
MAX_DATA_PAYLOAD_BYTES = 64 * 1024 * 1024

HELLO = 0x0001
INITIALIZE = 0x0002
INITIAL_STATE = 0x0003
READY = 0x0004
SHUTDOWN = 0x0005
ERROR = 0x00FF
STEP = 0x0101
RESET = 0x0102

REQUEST = 0x0001
RESPONSE = 0x0002
ERROR_FLAG = 0x0004
MORE_FRAMES = 0x0008


def canonical_json(value: Any) -> bytes:
    """Serialize one JSON-compatible value using the protocol canonical form.

    Match the production boundary for UTF-8 strings, safe integers, finite
    numbers, object ordering, and compact separators.
    """

    def encode(item: Any) -> str:
        """Recursively encode one JSON value using the test oracle rules."""

        if item is None:
            return "null"
        if item is True:
            return "true"
        if item is False:
            return "false"
        if isinstance(item, str):
            return json.dumps(item, ensure_ascii=False, separators=(",", ":"))
        if isinstance(item, int):
            if abs(item) > 2**53 - 1:
                raise ValueError("JSON integer exceeds the interoperable range")
            return str(item)
        if isinstance(item, float):
            if not math.isfinite(item):
                raise ValueError("non-finite JSON number")
            if item == 0.0:
                return "0"
            # Keep the oracle's number spelling aligned with production so
            # cross-language hashes test representation, not Python formatting.
            raw = repr(item).lower()
            if "e" not in raw:
                return raw[:-2] if raw.endswith(".0") else raw
            mantissa, exponent_text = raw.split("e")
            exponent = int(exponent_text)
            sign = "-" if mantissa.startswith("-") else ""
            digits = mantissa.removeprefix("-").replace(".", "")
            if -6 <= exponent < 21:
                decimal_at = 1 + exponent
                if decimal_at <= 0:
                    return sign + "0." + "0" * (-decimal_at) + digits
                if decimal_at >= len(digits):
                    return sign + digits + "0" * (decimal_at - len(digits))
                return sign + digits[:decimal_at] + "." + digits[decimal_at:]
            return f"{mantissa}e{'+' if exponent >= 0 else ''}{exponent}"
        if isinstance(item, list):
            return "[" + ",".join(encode(element) for element in item) + "]"
        if isinstance(item, dict):
            if not all(isinstance(key, str) for key in item):
                raise TypeError("JSON object keys must be strings")
            # The protocol orders UTF-16 code units, which differs for
            # supplementary Unicode characters.
            ordered = sorted(item, key=lambda key: key.encode("utf-16-be", errors="surrogatepass"))
            return "{" + ",".join(f"{encode(key)}:{encode(item[key])}" for key in ordered) + "}"
        raise TypeError(f"unsupported JSON value: {type(item).__name__}")

    return encode(value).encode("utf-8")


def sha256(value: Any) -> str:
    """Hash one canonical JSON value and return lowercase hexadecimal text.

    Args:
        value: JSON-compatible oracle value.

    Returns:
        Lowercase SHA-256 digest of the canonical bytes.
    """

    return hashlib.sha256(canonical_json(value)).hexdigest()


def strict_loads(payload: bytes) -> dict[str, Any]:
    """Decode one strict control object and reject duplicate or unsafe values."""

    def reject_duplicate(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        """Reject duplicate object keys instead of silently keeping the last value."""

        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def reject_constant(value: str) -> None:
        """Reject JSON NaN and infinity tokens."""

        raise ValueError(f"non-finite JSON number: {value}")

    value = json.loads(
        payload.decode("utf-8", errors="strict"), object_pairs_hook=reject_duplicate, parse_constant=reject_constant
    )
    if not isinstance(value, dict):
        raise ValueError("control payload must be a JSON object")

    def validate_numbers(item: Any) -> None:
        """Reject unsafe integers and non-finite numbers in nested containers."""

        if isinstance(item, bool) or item is None:
            return
        if isinstance(item, int):
            if abs(item) > 2**53 - 1:
                raise ValueError("JSON integer exceeds the interoperable range")
            return
        if isinstance(item, float):
            if not math.isfinite(item):
                raise ValueError("non-finite JSON number")
            return
        if isinstance(item, list):
            for element in item:
                validate_numbers(element)
        elif isinstance(item, dict):
            for element in item.values():
                validate_numbers(element)

    validate_numbers(value)
    return value


@dataclass(frozen=True)
class FrameHeader:
    """Represent the fixed 48-byte network-order frame header.

    Attributes:
        major: Protocol major version.
        minor: Protocol minor version.
        message: Protocol message kind.
        flags: Direction and optional error/continuation flags.
        payload_length: Bytes following the header.
        sequence: Request/response correlation number.
        session: Session UUID.
        layout_id: Data-plane layout identity, or zero for control messages.
    """

    major: int
    minor: int
    message: int
    flags: int
    payload_length: int
    sequence: int
    session: UUID
    layout_id: int = 0

    def pack(self) -> bytes:
        """Encode this header with the protocol magic and field ordering.

        Returns:
            Exactly 48 bytes in the shared big-endian header representation.
        """

        return HEADER.pack(
            MAGIC,
            self.major,
            self.minor,
            self.message,
            self.flags,
            self.payload_length,
            self.sequence,
            self.session.bytes,
            self.layout_id,
        )

    @classmethod
    def unpack(cls, data: bytes) -> FrameHeader:
        """Decode and validate one exact-size frame header.

        Args:
            data: Exactly 48 received header bytes.

        Returns:
            The typed header after magic, flag, and size validation.

        Raises:
            ValueError: If the frame is malformed.
        """

        if len(data) != HEADER_SIZE:
            raise ValueError("header must be exactly 48 bytes")
        magic, major, minor, message, flags, length, sequence, session, layout_id = HEADER.unpack(data)
        if magic != MAGIC:
            raise ValueError("frame magic mismatch")
        if flags & ~(REQUEST | RESPONSE | ERROR_FLAG | MORE_FRAMES):
            raise ValueError("unknown frame flags")
        if (flags & (REQUEST | RESPONSE)) not in (REQUEST, RESPONSE):
            raise ValueError("exactly one direction flag is required")
        return cls(major, minor, message, flags, length, sequence, UUID(bytes=session), layout_id)


@dataclass(frozen=True)
class Segment:
    """Describe one named field segment in a negotiated binary layout.

    Attributes:
        name: Stable field name.
        dtype: Little-endian scalar dtype.
        shape: Encoded batch shape.
        offset: Byte offset in the payload.
        byte_length: Encoded segment length.
    """

    name: str
    dtype: str
    shape: tuple[int, ...]
    offset: int
    byte_length: int


@dataclass(frozen=True)
class Layout:
    """Represent one validated field-major binary layout and its identity.

    The test oracle intentionally mirrors the production identity calculation;
    changing descriptor order must change both hash and numeric layout ID.
    """

    kind: str
    payload_length: int
    layout_id: int
    layout_hash: str
    segments: tuple[Segment, ...]

    @classmethod
    def parse(cls, descriptor: dict[str, Any], batch_size: int) -> Layout:
        """Parse one descriptor and verify its canonical hash and layout ID.

        Args:
            descriptor: External layout DTO.
            batch_size: Slot count included in the identity calculation.

        Returns:
            A typed immutable layout descriptor.

        Raises:
            ValueError: If the canonical hash or derived layout ID differs.
        """

        segments = tuple(
            Segment(item["name"], item["dtype"], tuple(item["shape"]), item["offset"], item["byte_length"])
            for item in descriptor["segments"]
        )
        hash_input = {
            "protocol_version": {"major": 2, "minor": 0},
            "layout_kind": descriptor["layout_kind"],
            "batch_size": batch_size,
            "byte_order": descriptor["byte_order"],
            "alignment": descriptor["alignment"],
            "ordered_segments": descriptor["segments"],
        }
        expected_hash = sha256(hash_input)
        if descriptor["layout_hash"] != expected_hash:
            raise ValueError(f"layout hash mismatch for {descriptor['layout_kind']}")
        expected_id = int(expected_hash[:16], 16)
        if descriptor["layout_id"] != f"{expected_id:016x}":
            raise ValueError(f"layout id mismatch for {descriptor['layout_kind']}")
        return cls(descriptor["layout_kind"], descriptor["payload_length"], expected_id, expected_hash, segments)

    def segment(self, name: str) -> Segment:
        """Return the named segment from this validated layout.

        Args:
            name: Exact negotiated field name.

        Returns:
            The matching segment descriptor.

        Raises:
            StopIteration: If the field is absent.
        """

        return next(segment for segment in self.segments if segment.name == name)


def generic_cartpole_robot_spec() -> RobotSpec:
    """Bind canonical CartPole semantics to the stable test-content topology."""

    topology = RobotTopology.from_response(
        {
            "asset_path": "/Game/Robots/CartPole/SKM_CartPole",
            "body_names": ["base", "cart", "pole"],
            "body_motion_types": ["kinematic", "simulated", "simulated"],
            "root_body_index": 0,
            "fixed_base": True,
            "joints": [
                {
                    "name": "cart",
                    "parent_body_index": 0,
                    "child_body_index": 1,
                    "degrees_of_freedom": 1,
                    "coordinate": "linear_x",
                    "coordinate_type": "prismatic",
                    "unit": "m",
                    "default_position": 0.0,
                    "lower_limit": None,
                    "upper_limit": None,
                    "child_frame": {
                        "position_metres": [0.0, 0.0, 0.0],
                        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "parent_frame": {
                        "position_metres": [0.0, 0.0, 0.0],
                        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                },
                {
                    "name": "pole",
                    "parent_body_index": 1,
                    "child_body_index": 2,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -3.14,
                    "upper_limit": 3.14,
                    "child_frame": {
                        "position_metres": [0.0, 0.0, 0.1],
                        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "parent_frame": {
                        "position_metres": [0.0, 0.0, 0.1],
                        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                },
            ],
        }
    )
    return merge_robot_spec(load_cartpole_training_config().robot_config, topology)


def generic_cartpole_schema() -> dict[str, list[dict[str, Any]]]:
    """Return the generic RobotSpec-driven CartPole wire schema."""

    spec = generic_cartpole_robot_spec()
    observation_shapes = ObservationShapeTable(
        {
            ObsType.JOINT_POSITION: (),
            ObsType.JOINT_VELOCITY: (),
        }
    )
    return SessionSchema(
        robot_observation_schema(spec, observation_shapes),
        robot_actuator_action_schema(spec),
    ).as_request()


def generic_cartpole_worker_config(num_slots: int, seed: int = 0) -> dict[str, Any]:
    """Build the Generic Robot Worker Config used by SocketBridge tests.

    Args:
        num_slots: Positive parallel Slot count.
        seed: Deterministic Worker Run seed encoded as a string at the wire
            boundary.

    Returns:
        A fresh JSON-compatible Worker projection with fixed physics and
        Cart-Pole Environment and generic SkeletalMesh Robot identifiers.
    """
    training = load_cartpole_training_config()
    spec = generic_cartpole_robot_spec()
    return {
        "num_slots": num_slots,
        "physics_dt": 1.0 / 120.0,
        "decimation": [2, 2],
        "run_seed": str(seed),
        "world_map": "/Engine/Maps/Entry",
        "environment": {
            "id": "uerl.environment.shared_world",
            "scalars": dict(training.worker.environment_config),
            "terrain": to_jsonable(training.worker.terrain_config),
        },
        "robot": {
            "id": "uerl.robot.skeletal_mesh",
            "asset_path": training.robot_asset_path,
            "scalars": {},
            "actuators": [
                {
                    "index": actuator.index,
                    "joint_index": actuator.joint_index,
                    "joint": actuator.joint,
                    "coordinate": actuator.coordinate,
                    "coordinate_type": actuator.coordinate_type,
                    "unit": actuator.target_unit,
                    "target_mode": actuator.target_mode,
                    "stiffness": actuator.stiffness,
                    "damping": actuator.damping,
                    "effort_limit": actuator.effort_limit,
                    "default_pos": actuator.default_pos,
                }
                for actuator in spec.actuators
            ],
            "reset_bindings": [
                {
                    "index": target.index,
                    "name": target.wire_key,
                    "target_type": target.target_type.value,
                    "joint_index": target.joint_index if target.joint_index is not None else -1,
                    "body_index": target.body_index if target.body_index is not None else -1,
                    "component_index": target.component_index,
                    "coordinate": (
                        spec.topology.joints[target.joint_index].coordinate
                        if target.joint_index is not None
                        else "root"
                    ),
                    "unit": target.unit,
                }
                for target in spec.reset
            ],
        },
    }


def encode_action(
    layout: Layout,
    schema: list[dict[str, Any]],
    actions: np.ndarray,
    *,
    step_decimation: int = 1,
) -> bytes:
    """Encode row-major Action values into the negotiated field-major layout.

    Args:
        layout: Validated Step-action layout.
        schema: Ordered action fields matching policy columns.
        actions: Finite ``(num_slots, action_width)`` row-major values.

    Returns:
        Exact little-endian payload bytes with one contiguous segment per field.
    """

    payload = bytearray(layout.payload_length)
    column = 0
    for field in schema:
        # The policy matrix is row-major, while the wire contract stores each
        # named field contiguously across Slots.
        segment = layout.segment(field["name"])
        width = math.prod(field["shape"]) if field["shape"] else 1
        values = np.asarray(actions[:, column : column + width], dtype="<f4", order="C")
        payload[segment.offset : segment.offset + segment.byte_length] = values.tobytes(order="C")
        column += width
    decimation = layout.segment("step_decimation")
    if decimation.dtype != "int32" or decimation.shape != (1,) or step_decimation <= 0:
        raise ValueError("step_decimation segment or value is invalid")
    payload[decimation.offset : decimation.offset + decimation.byte_length] = np.asarray(
        [step_decimation], dtype="<i4"
    ).tobytes()
    return bytes(payload)


def encode_reset_mask(layout: Layout, num_slots: int, slots: list[int]) -> bytes:
    """Encode selected Slot IDs into the protocol's little-endian bitset.

    Args:
        layout: Validated Reset-request layout containing ``system.reset_mask``.
        num_slots: Negotiated Slot count used for bounds checking.
        slots: Selected Slot IDs; duplicates set the same bit without changing
            the request.

    Returns:
        Exact payload bytes with unused high bits left clear.
    """

    payload = bytearray(layout.payload_length)
    mask = layout.segment("system.reset_mask")
    for slot in slots:
        if slot < 0 or slot >= num_slots:
            raise ValueError(f"reset slot out of range: {slot}")
        # Little-endian bit numbering makes Slot zero the low bit of byte zero.
        payload[mask.offset + slot // 8] |= 1 << (slot % 8)
    return bytes(payload)


def decode_state(
    layout: Layout, schema: list[dict[str, Any]], payload: bytes
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Decode State fields, Slot faults, and episode indices from one payload.

    Args:
        layout: Validated state-result layout.
        schema: Ordered state fields whose columns must match the returned
            row-major matrix.
        payload: Exact binary response payload.

    Returns:
        A tuple of row-major state values, uint16 fault codes, and uint64
        episode indices. Missing system metadata is represented by zero arrays
        for compatibility with initial frames.
    """

    if len(payload) != layout.payload_length:
        raise ValueError("state payload length mismatch")
    num_slots = layout.segments[0].shape[0]
    width = sum(math.prod(field["shape"]) if field["shape"] else 1 for field in schema)
    state = np.zeros((num_slots, width), dtype=np.float32)
    column = 0
    for field in schema:
        # Rebuild policy columns from field-major wire segments while retaining
        # the schema's declared observation order.
        segment = layout.segment(field["name"])
        field_width = math.prod(field["shape"]) if field["shape"] else 1
        values = np.frombuffer(payload, dtype="<f4", count=num_slots * field_width, offset=segment.offset)
        state[:, column : column + field_width] = values.reshape(num_slots, field_width)
        column += field_width
    if not np.isfinite(state).all():
        raise ValueError("non-finite state payload")
    try:
        fault_segment = layout.segment("system.slot_fault_code")
        faults = np.frombuffer(payload, dtype="<u2", count=num_slots, offset=fault_segment.offset).copy()
    except StopIteration:
        faults = np.zeros(num_slots, dtype=np.uint16)
    try:
        episode_segment = layout.segment("system.episode_index")
        episodes = np.frombuffer(payload, dtype="<u8", count=num_slots, offset=episode_segment.offset).copy()
    except StopIteration:
        episodes = np.zeros(num_slots, dtype=np.uint64)
    return state, faults, episodes

"""Verify the formal Python U4 wire codec against golden behavior."""

from uuid import UUID

import pytest

from uerl.core.codec import (
    HEADER_SIZE,
    REQUEST,
    STEP,
    BatchCodec,
    BridgeProtocolError,
    FrameHeader,
    Layout,
    canonical_json,
    sha256_json,
    strict_loads,
)

GOLDEN_HEADER_HEX = (
    "5545524c0002000001010001000000100000000000000007"
    "00112233445546778899aabbccddeeff1020304050607080"
)
GOLDEN_JCS_HASH = "cdab067e9f3beb32d1252cfd63e492592fecbf591b0d08cadb24bb17f3864246"


def test_frame_header_matches_cross_language_golden() -> None:
    """Keep the U4 frame layout and byte order stable."""

    header = FrameHeader(
        major=2,
        minor=0,
        message=STEP,
        flags=REQUEST,
        payload_length=16,
        sequence=7,
        session=UUID("00112233-4455-4677-8899-aabbccddeeff"),
        layout_id=0x1020304050607080,
    )

    encoded = header.pack()

    assert len(encoded) == HEADER_SIZE
    assert encoded.hex() == GOLDEN_HEADER_HEX
    assert FrameHeader.unpack(encoded) == header


def test_canonical_hash_matches_cross_language_golden() -> None:
    """Keep canonical JSON ordering and number formatting stable."""

    assert sha256_json({"b": 1, "a": "x"}) == GOLDEN_JCS_HASH
    assert canonical_json([1e30, 4.50, 2e-3, 1e-27]) == b"[1e+30,4.5,0.002,1e-27]"
    assert canonical_json({"\ufffd": 1, "\U0001f600": 2}) == '{"😀":2,"�":1}'.encode()


def test_strict_json_rejects_duplicate_and_non_finite_values() -> None:
    """Reject malformed control payloads before they enter domain code."""

    with pytest.raises(BridgeProtocolError, match="duplicate"):
        strict_loads(b'{"a":1,"a":2}')
    with pytest.raises(BridgeProtocolError, match="non-finite"):
        strict_loads(b'{"value":NaN}')
    with pytest.raises(BridgeProtocolError, match="interoperable"):
        strict_loads(b'{"value":9007199254740992}')


def test_header_rejects_unknown_flags() -> None:
    """Reject unknown frame flags at the external frame boundary."""

    malformed = bytearray.fromhex(GOLDEN_HEADER_HEX)
    malformed[11] |= 0x10

    with pytest.raises(BridgeProtocolError, match="unknown frame flags"):
        FrameHeader.unpack(bytes(malformed))


def test_layout_validates_hash_and_layout_id() -> None:
    """Validate a generic descriptor without importing task-specific schema."""

    descriptor = {
        "layout_kind": "initial_state",
        "byte_order": "little",
        "alignment": 8,
        "payload_length": 0,
        "layout_id": "",
        "layout_hash": "",
        "segments": [],
    }
    hash_input = {
        "protocol_version": {"major": 2, "minor": 0},
        "layout_kind": descriptor["layout_kind"],
        "batch_size": 1,
        "byte_order": descriptor["byte_order"],
        "alignment": descriptor["alignment"],
        "ordered_segments": descriptor["segments"],
    }
    layout_hash = sha256_json(hash_input)
    descriptor["layout_hash"] = layout_hash
    descriptor["layout_id"] = layout_hash[:16]

    layout = Layout.parse(descriptor, batch_size=1)

    assert layout.payload_length == 0
    assert layout.layout_hash == descriptor["layout_hash"]
    print("[VERIFY] VC-005: protocol=U4 sequence=PASS layout_hash=PASS")


def test_layout_rejects_payload_length_not_explained_by_segments() -> None:
    """Reject a descriptor whose declared bytes do not match its segments."""

    descriptor = {
        "layout_kind": "initial_state",
        "byte_order": "little",
        "alignment": 8,
        "payload_length": 8,
        "layout_id": "",
        "layout_hash": "",
        "segments": [],
    }
    hash_input = {
        "protocol_version": {"major": 2, "minor": 0},
        "layout_kind": descriptor["layout_kind"],
        "batch_size": 1,
        "byte_order": descriptor["byte_order"],
        "alignment": descriptor["alignment"],
        "ordered_segments": descriptor["segments"],
    }
    layout_hash = sha256_json(hash_input)
    descriptor["layout_hash"] = layout_hash
    descriptor["layout_id"] = layout_hash[:16]

    with pytest.raises(BridgeProtocolError, match="payload length"):
        Layout.parse(descriptor, batch_size=1)


def test_batch_codec_rejects_non_finite_float_payload() -> None:
    """Reject non-finite values before a batch reaches task code."""

    descriptor = {
        "layout_kind": "step_result",
        "byte_order": "little",
        "alignment": 8,
        "payload_length": 4,
        "layout_id": "",
        "layout_hash": "",
        "segments": [{"name": "state.value", "dtype": "float32", "shape": [1], "offset": 0, "byte_length": 4}],
    }
    hash_input = {
        "protocol_version": {"major": 2, "minor": 0},
        "layout_kind": descriptor["layout_kind"],
        "batch_size": 1,
        "byte_order": descriptor["byte_order"],
        "alignment": descriptor["alignment"],
        "ordered_segments": descriptor["segments"],
    }
    layout_hash = sha256_json(hash_input)
    descriptor["layout_hash"] = layout_hash
    descriptor["layout_id"] = layout_hash[:16]
    layout = Layout.parse(descriptor, batch_size=1)

    with pytest.raises(BridgeProtocolError, match="non-finite"):
        BatchCodec.validate_and_map(layout, bytes.fromhex("0000c07f"))

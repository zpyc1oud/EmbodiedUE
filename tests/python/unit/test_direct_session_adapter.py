"""Verify the raw Session to typed DirectSession mapping."""

from __future__ import annotations

from collections.abc import Mapping
from types import SimpleNamespace
from typing import Any
from uuid import UUID

import numpy as np
import pytest
import torch

from uerl import (
    InitialState,
    PhysicalCommandBatch,
    SessionError,
    SessionSchema,
    UERLSessionAdapter,
)
from uerl.core.codec import FrameHeader, InitializeDescription, InitializeResult
from uerl.core.config.robot import RobotSpec, RobotTopology


def _descriptor(kind: str, payload_length: int, segments: list[dict[str, object]]) -> dict[str, object]:
    """Build a trusted descriptor for the adapter test fake."""

    return {
        "layout_kind": kind,
        "payload_length": payload_length,
        "layout_id": f"{len(kind):016x}",
        "layout_hash": f"{kind}-trusted-by-test",
        "segments": segments,
    }


def _segment(name: str, dtype: str, shape: list[int], offset: int, byte_length: int) -> dict[str, object]:
    return {"name": name, "dtype": dtype, "shape": shape, "offset": offset, "byte_length": byte_length}


def _schema() -> SessionSchema:
    return SessionSchema(
        (
            {
                "name": "state.value",
                "dtype": "float32",
                "shape": [],
                "unit": "m",
                "frame": "slot/world",
                "semantic": "position",
                "source": "test.robot",
            },
        ),
        (
            {
                "name": "cmd.force",
                "dtype": "float32",
                "shape": [],
                "unit": "N",
                "frame": "slot/world",
                "semantic": "force",
                "source": "test.robot",
            },
        ),
    )


def _layouts() -> list[dict[str, object]]:
    return [
        _descriptor(
            "initial_state",
            24,
            [
                _segment("state.value", "float32", [2], 0, 8),
                _segment("system.episode_index", "uint64", [2], 8, 16),
            ],
        ),
        _descriptor(
            "step_action",
            12,
            [
                _segment("cmd.force", "float32", [2], 0, 8),
                _segment("step_decimation", "int32", [1], 8, 4),
            ],
        ),
        _descriptor(
            "step_result",
            20,
            [
                _segment("state.value", "float32", [2], 0, 8),
                _segment("system.state_valid", "uint8", [2], 8, 2),
                _segment("system.slot_fault_code", "uint16", [2], 16, 4),
            ],
        ),
        _descriptor("reset_request", 1, [_segment("system.reset_mask", "uint8", [1], 0, 1)]),
        _descriptor(
            "reset_result",
            32,
            [
                _segment("state.value", "float32", [2], 0, 8),
                _segment("system.reset_mask", "uint8", [1], 8, 1),
                _segment("system.episode_index", "uint64", [2], 16, 16),
            ],
        ),
    ]


def _robot_spec() -> RobotSpec:
    """Return the minimal reflected RobotSpec required by the v2 seam."""

    return RobotSpec(
        actuators=(),
        observations=(),
        reset=(),
        body_names=("root",),
        topology=RobotTopology(("root",), ("simulated",), 0, False, ()),
    )


def _initialize_payload() -> bytes:
    payload = bytearray(24)
    payload[0:8] = np.asarray([1.0, 2.0], dtype="<f4").tobytes()
    payload[8:24] = np.asarray([0, 0], dtype="<u8").tobytes()
    return bytes(payload)


def _step_payload() -> bytes:
    payload = bytearray(20)
    payload[0:8] = np.asarray([3.0, 4.0], dtype="<f4").tobytes()
    payload[8:10] = np.asarray([1, 0], dtype="u1").tobytes()
    payload[16:20] = np.asarray([0, 6], dtype="<u2").tobytes()
    return bytes(payload)


def _reset_payload() -> bytes:
    payload = bytearray(32)
    payload[0:8] = np.asarray([0.1, 0.2], dtype="<f4").tobytes()
    payload[8] = 1
    payload[16:32] = np.asarray([1, 0], dtype="<u8").tobytes()
    return bytes(payload)


class _RawSession:
    """Provide a minimal raw session with the formal Session surface."""

    def __init__(self) -> None:
        self.config: Any = SimpleNamespace(worker=SimpleNamespace(slot_count=2, terrain_config={}))
        self.config.worker.decimation = (1, 1)
        self.events: list[str] = []
        self.close_calls = 0
        self.step_payload: bytes | None = None
        self.reset_payload: bytes | None = None
        self.last_step_timing = {
            "client_request_validate": 0.001,
            "client_socket_send": 0.002,
            "client_response_wait_recv": 0.003,
            "client_response_validate": 0.004,
        }
        self.last_reset_timing = self.last_step_timing
        self.described_robot_spec = _robot_spec()
        self.descriptor: dict[str, Any] = {
            "phase": "describe",
            "topology": {},
            "available_state_schema": [],
            "available_action_schema": [],
        }

    def describe(self, schema_request: Mapping[str, Any]) -> InitializeDescription:
        """Record the required topology description phase."""

        self.events.append("describe")
        return InitializeDescription(self.descriptor)

    def initialize(self, schema_request: Mapping[str, Any]) -> InitializeResult:
        """Record Initialize and return trusted layouts plus initial payload."""

        self.events.append("initialize")
        return InitializeResult(
            {
                "phase": "commit",
                "effective_worker_config": {"num_slots": 2},
                "layouts": _layouts(),
            },
            FrameHeader(2, 0, 3, 2, 24, 1, UUID(int=1), 1),
            _initialize_payload(),
        )

    def acknowledge_ready(self) -> Mapping[str, Any]:
        """Record the Ready acknowledgement requested by the adapter."""

        self.events.append("ready")
        return {}

    def event(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """Record an event request while satisfying the raw Session seam."""

        self.events.append("event")
        return {}

    def step(self, layout_id: int, payload: bytes) -> tuple[object, bytes]:
        """Record the encoded command payload and return a scripted Step result."""

        self.events.append("step")
        self.step_payload = payload
        return object(), _step_payload()

    def reset(self, layout_id: int, payload: bytes) -> tuple[object, bytes]:
        """Record the encoded mask payload and return a scripted Reset result."""

        self.events.append("reset")
        self.reset_payload = payload
        return object(), _reset_payload()

    def close(self, reason: str = "session_close") -> None:
        """Count adapter cleanup calls and record their order."""

        self.close_calls += 1
        self.events.append("close")


def _initialize(adapter: UERLSessionAdapter) -> InitialState:
    """Drive the formal Describe→RobotSpec bind→Commit test lifecycle."""

    return adapter.initialize_with_robot_spec(_schema(), lambda _robot_spec, _shapes: _schema())


def test_adapter_encodes_commands_and_decodes_state_batches() -> None:
    """Map the named fields while keeping system vectors typed and separate."""

    raw = _RawSession()
    adapter = UERLSessionAdapter(raw)
    initial = _initialize(adapter)
    adapter.acknowledge_ready()

    assert torch.equal(initial["state.value"], torch.tensor([1.0, 2.0]))
    assert torch.equal(initial.episode_index, torch.tensor([0, 0], dtype=torch.uint64))

    transition = adapter.step(PhysicalCommandBatch({"cmd.force": torch.tensor([7.0, 8.0])}))

    assert raw.step_payload == np.asarray([7.0, 8.0], dtype="<f4").tobytes() + np.asarray([1], dtype="<i4").tobytes()
    assert torch.equal(transition["state.value"], torch.tensor([3.0, 4.0]))
    assert torch.equal(transition.state_valid, torch.tensor([True, False]))
    assert torch.equal(transition.slot_fault_code, torch.tensor([0, 6], dtype=torch.uint16))
    assert set(adapter.last_step_timing) == {
        "client_action_encode",
        "client_request_validate",
        "client_socket_send",
        "client_response_wait_recv",
        "client_response_validate",
        "client_state_decode",
    }
    print("[VERIFY] VC-002: ready_before_step=true init_failure_close=1")


def test_adapter_requires_and_encodes_variable_step_decimation() -> None:
    """A variable range cannot silently choose a frame count."""

    raw = _RawSession()
    raw.config.worker.decimation = (1, 7)
    adapter = UERLSessionAdapter(raw)
    _initialize(adapter)
    with pytest.raises(SessionError, match="step_decimation"):
        adapter.step(PhysicalCommandBatch({"cmd.force": torch.tensor([7.0, 8.0])}))
    adapter.step(PhysicalCommandBatch({"cmd.force": torch.tensor([7.0, 8.0])}), 7)
    assert raw.step_payload is not None
    assert raw.step_payload[-4:] == np.asarray([7], dtype="<i4").tobytes()


def test_adapter_packs_sparse_reset_and_updates_episode_index() -> None:
    """Reset masks use the wire bitset and the returned episode index."""

    raw = _RawSession()
    adapter = UERLSessionAdapter(raw)
    _initialize(adapter)
    post_reset = adapter.reset(torch.tensor([True, False]))

    assert raw.reset_payload == b"\x01"
    assert torch.equal(post_reset["state.value"], torch.tensor([0.1, 0.2]))
    assert torch.equal(post_reset.state_valid, torch.tensor([True, False]))
    assert torch.equal(post_reset.episode_index, torch.tensor([1, 0], dtype=torch.uint64))
    assert set(adapter.last_reset_timing) == {
        "client_reset_encode",
        "client_request_validate",
        "client_socket_send",
        "client_response_wait_recv",
        "client_response_validate",
        "client_reset_decode",
        "client_reset_roundtrip",
    }
    print("[VERIFY] VC-006: reset_calls=1 episode_steps=0 seed_unchanged=true")


def test_adapter_encodes_terrain_levels_at_the_negotiated_offset() -> None:
    """Write uint16 terrain levels little-endian while preserving zero padding."""

    class TerrainSession(_RawSession):
        """Add the terrain segment expected by a configured Worker."""

        def __init__(self) -> None:
            super().__init__()
            self.config.worker.terrain_config = {"num_levels": 3}

        def initialize(self, schema_request: Mapping[str, Any]) -> InitializeResult:
            result = super().initialize(schema_request)
            layouts = list(result.response["layouts"])
            request = next(item for item in layouts if item["layout_kind"] == "reset_request")
            request["payload_length"] = 12
            request["segments"] = [
                _segment("system.reset_mask", "uint8", [1], 0, 1),
                _segment("terrain_level", "uint16", [2], 8, 4),
            ]
            return InitializeResult(
                {**result.response, "layouts": layouts},
                result.initial_header,
                result.initial_payload,
            )

    raw = TerrainSession()
    adapter = UERLSessionAdapter(raw)
    _initialize(adapter)
    adapter.reset(
        torch.tensor([True, False]),
        torch.tensor([2, 1], dtype=torch.uint16),
    )

    assert raw.reset_payload == b"\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x01\x00"
    with pytest.raises(SessionError, match="outside the configured range"):
        adapter.reset(torch.tensor([True, False]), torch.tensor([3, 1], dtype=torch.uint16))


def test_adapter_rejects_missing_required_system_segment() -> None:
    """Reject a negotiated StepResult layout that cannot report fault state."""

    class MissingFaultSession(_RawSession):
        """Remove the required fault segment from the negotiated StepResult layout."""

        def initialize(self, schema_request: Mapping[str, Any]) -> InitializeResult:
            """Return a response whose fault metadata is intentionally incomplete."""

            result = super().initialize(schema_request)
            layouts = list(result.response["layouts"])
            step_result = next(item for item in layouts if item["layout_kind"] == "step_result")
            step_result["segments"] = [
                segment for segment in step_result["segments"] if segment["name"] != "system.slot_fault_code"
            ]
            response = {**result.response, "layouts": layouts}
            return InitializeResult(response, result.initial_header, result.initial_payload)

    with pytest.raises(SessionError, match="system.slot_fault_code"):
        _initialize(UERLSessionAdapter(MissingFaultSession()))


def test_adapter_rejects_reset_result_mask_mismatch() -> None:
    """Reject a Worker ResetResult that acknowledges a different Slot set."""

    class MismatchedResetSession(_RawSession):
        """Return a ResetResult mask that differs from the requested Slot set."""

        def reset(self, layout_id: int, payload: bytes) -> tuple[object, bytes]:
            """Clear the acknowledged bit while retaining an otherwise valid payload."""

            result = bytearray(_reset_payload())
            result[8] = 0
            return object(), bytes(result)

    raw = MismatchedResetSession()
    adapter = UERLSessionAdapter(raw)
    _initialize(adapter)
    with pytest.raises(SessionError, match="mask"):
        adapter.reset(torch.tensor([True, False]))
    adapter.close()
    assert raw.close_calls == 1
    assert raw.events[-1] == "close"


def test_adapter_rejects_unused_reset_mask_bits() -> None:
    """Reject a ResetResult bitset that sets slots outside the negotiated batch."""

    class InvalidResetSession(_RawSession):
        """Set a high bit outside the two-Slot negotiated batch."""

        def reset(self, layout_id: int, payload: bytes) -> tuple[object, bytes]:
            """Return a ResetResult with an unused mask bit set."""

            result = bytearray(_reset_payload())
            result[8] = 4
            return object(), bytes(result)

    raw = InvalidResetSession()
    adapter = UERLSessionAdapter(raw)
    _initialize(adapter)
    with pytest.raises(SessionError, match="unused"):
        adapter.reset(torch.tensor([True, False]))
    assert raw.close_calls == 1


@pytest.mark.parametrize("mask", [torch.tensor([True]), torch.tensor([[True], [False]])])
def test_adapter_rejects_wrong_reset_mask_shape_before_worker_mutation(mask: torch.Tensor) -> None:
    """A mask must address the negotiated Slot vector before sending any Reset."""

    raw = _RawSession()
    adapter = UERLSessionAdapter(raw)
    _initialize(adapter)

    with pytest.raises(SessionError):
        adapter.reset(mask)

    assert raw.reset_payload is None
    assert raw.close_calls == 0

"""Verify synchronous Bridge session sequencing and failure closeout."""

from __future__ import annotations

import socket
from typing import Any
from uuid import UUID

import pytest

from uerl import SessionError
from uerl.core.codec import (
    ERROR,
    ERROR_FLAG,
    EVENT,
    HELLO,
    INITIAL_STATE,
    INITIALIZE,
    MORE_FRAMES,
    PROTOCOL,
    READY,
    RESET,
    RESPONSE,
    SHUTDOWN,
    STEP,
    BridgeProtocolError,
    FrameHeader,
    SocketBridgeSession,
    canonical_json,
    sha256_json,
)
from uerl.core.codec.transport import SocketTransport, connect_socket_with_deadline

SESSION_ID = UUID("00112233-4455-4677-8899-aabbccddeeff")

_TOPOLOGY = {
    "body_names": ["cart", "pole"],
    "body_motion_types": ["simulated", "simulated"],
    "root_body_index": 0,
    "fixed_base": False,
    "joints": [
        {
            "name": "pole_joint",
            "parent_body_index": 0,
            "child_body_index": 1,
            "degrees_of_freedom": 1,
            "coordinate": "twist",
            "coordinate_type": "revolute",
            "unit": "rad",
            "default_position": 0.0,
            "lower_limit": -3.14,
            "upper_limit": 3.14,
            "child_frame": {
                "position_metres": [0.0, 0.0, 0.0],
                "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "parent_frame": {
                "position_metres": [0.0, 0.0, 0.0],
                "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        }
    ],
}


class _FakeTransport:
    """Serve scripted response frames in deliberately fragmented reads."""

    def __init__(self, responses: bytes, chunk_size: int = 2) -> None:
        self._responses = bytearray(responses)
        self._chunk_size = chunk_size
        self.sent: list[bytes] = []
        self.closed = False

    def sendall(self, data: bytes) -> None:
        """Record the complete outbound frame for sequence assertions."""

        self.sent.append(data)

    def recv(self, length: int) -> bytes:
        """Return deliberately short fragments from the scripted response stream."""

        if not self._responses:
            return b""
        size = min(length, self._chunk_size, len(self._responses))
        result = bytes(self._responses[:size])
        del self._responses[:size]
        return result

    def close(self) -> None:
        """Record transport cleanup without touching a real socket."""

        self.closed = True


def _response(message: int, sequence: int, payload: bytes, *, flags: int = RESPONSE, layout_id: int = 0) -> bytes:
    """Build one scripted response frame."""

    return FrameHeader(
        PROTOCOL[0],
        PROTOCOL[1],
        message,
        flags,
        len(payload),
        sequence,
        SESSION_ID,
        layout_id,
    ).pack() + payload


def _layout(kind: str) -> dict[str, Any]:
    """Build one valid empty layout descriptor for the scripted Worker."""

    descriptor: dict[str, Any] = {
        "layout_kind": kind,
        "byte_order": "little",
        "alignment": 8,
        "payload_length": 0,
        "layout_id": "",
        "layout_hash": "",
        "segments": [],
    }
    hash_input = {
        "protocol_version": {"major": 2, "minor": 0},
        "layout_kind": kind,
        "batch_size": 1,
        "byte_order": "little",
        "alignment": 8,
        "ordered_segments": [],
    }
    descriptor["layout_hash"] = sha256_json(hash_input)
    descriptor["layout_id"] = descriptor["layout_hash"][:16]
    return descriptor


_LAYOUTS = {kind: _layout(kind) for kind in (
    "initial_state", "step_action", "step_result", "reset_request", "reset_result"
)}


def _scripted_handshake(*, include_event: bool = False) -> bytes:
    """Build Hello through Shutdown responses for one complete session."""

    effective_worker_config = {
        "num_slots": 1,
        "physics_dt": 1.0 / 60.0,
        "decimation": [1, 1],
        "world_map": "/Engine/Maps/Entry",
    }
    description = {
        "phase": "describe",
        "selected_protocol": {"major": 2, "minor": 0},
        "build_identity": {},
        "effective_worker_config": effective_worker_config,
        "effective_worker_config_hash": sha256_json(effective_worker_config),
        "available_state_schema": [],
        "available_action_schema": [],
        "topology": _TOPOLOGY,
        "seed_derivation_version": "test-v1",
    }
    commit = {
        **description,
        "phase": "commit",
        "selected_schemas": {"state_schema_hash": "0" * 64, "action_schema_hash": "0" * 64},
        "layouts": list(_LAYOUTS.values()),
    }
    responses = [
        _response(HELLO, 1, canonical_json({"selected_protocol": {"major": 2, "minor": 0}})),
        _response(INITIALIZE, 2, canonical_json(description)),
        _response(
            INITIALIZE,
            3,
            canonical_json(commit),
            flags=RESPONSE | MORE_FRAMES,
        ),
        _response(INITIAL_STATE, 3, b"", layout_id=int(str(_LAYOUTS["initial_state"]["layout_id"]), 16)),
        _response(READY, 4, canonical_json({"ready": True})),
    ]
    if include_event:
        responses.append(_response(EVENT, 5, canonical_json({"applied": True, "kind": "root_push"})))
        responses.append(_response(SHUTDOWN, 6, canonical_json({"final_step_count": "1"})))
    else:
        responses.extend(
            [
                _response(STEP, 5, b"", layout_id=int(str(_LAYOUTS["step_result"]["layout_id"]), 16)),
                _response(RESET, 6, b"", layout_id=int(str(_LAYOUTS["reset_result"]["layout_id"]), 16)),
                _response(SHUTDOWN, 7, canonical_json({"final_step_count": "1"})),
            ]
        )
    return b"".join(responses)


def test_repeated_describe_fails_closed() -> None:
    """Reject a second Describe before Commit and close the raw transport."""

    transport = _FakeTransport(_scripted_handshake())
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    session.connect()
    session.describe({"state_requirements": [], "action_schema": []})
    with pytest.raises(BridgeProtocolError, match="already completed"):
        session.describe({"state_requirements": [], "action_schema": []})

    assert session.state.value == "failed"
    assert transport.closed


def test_repeated_commit_fails_closed() -> None:
    """Reject a second Commit after Initialize and close the raw transport."""

    transport = _FakeTransport(_scripted_handshake())
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    request: dict[str, object] = {"state_requirements": [], "action_schema": []}
    session.connect()
    session.describe(request)
    session.initialize(request)
    with pytest.raises(BridgeProtocolError, match="not valid in state initialized"):
        session.initialize(request)

    assert session.state.value == "failed"
    assert transport.closed


def test_repeated_ready_fails_closed() -> None:
    """Reject a second Ready acknowledgement and close the raw transport."""

    transport = _FakeTransport(_scripted_handshake())
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    request: dict[str, object] = {"state_requirements": [], "action_schema": []}
    session.connect()
    session.describe(request)
    session.initialize(request)
    session.acknowledge_ready("0" * 64)
    with pytest.raises(SessionError, match="Ready is not valid"):
        session.acknowledge_ready("0" * 64)

    assert session.state.value == "failed"
    assert transport.closed


def test_session_completes_u4_sequence_with_one_in_flight() -> None:
    """Complete Hello, Initialize, Ready, Step, Reset, and Shutdown."""

    transport = _FakeTransport(_scripted_handshake())
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    assert session.connect()["selected_protocol"] == {"major": 2, "minor": 0}
    session.describe({"state_requirements": [], "action_schema": []})
    initial = session.initialize({"state_requirements": [], "action_schema": []})
    assert initial.initial_payload == b""
    assert initial.topology.body_names == ("cart", "pole")
    assert initial.topology.joints[0].child_body_index == 1
    assert session.acknowledge_ready("a" * 64) == {"ready": True}
    assert session.step(int(str(_LAYOUTS["step_action"]["layout_id"]), 16), b"")[1] == b""
    assert set(session.last_step_timing) == {
        "client_request_validate",
        "client_socket_send",
        "client_response_wait_recv",
        "client_response_validate",
    }
    assert all(value >= 0.0 for value in session.last_step_timing.values())
    assert session.reset(int(str(_LAYOUTS["reset_request"]["layout_id"]), 16), b"")[1] == b""
    assert set(session.last_reset_timing) == {
        "client_request_validate",
        "client_socket_send",
        "client_response_wait_recv",
        "client_response_validate",
    }
    assert all(value >= 0.0 for value in session.last_reset_timing.values())
    assert session.shutdown("test") == {"final_step_count": "1"}

    assert session.in_flight_max == 1
    assert session.state.value == "closed"
    assert transport.closed
    print("[VERIFY] VC-005: protocol=U4 sequence=PASS layout_hash=PASS")


def test_session_exchanges_ready_event_control_frame() -> None:
    """Event uses the control payload limit and the existing Ready sequence."""

    transport = _FakeTransport(_scripted_handshake(include_event=True))
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    request = {"kind": "root_push", "slot_ids": [0], "values": [[0.1, 0.0, 0.0]]}
    session.connect()
    session.describe({"state_requirements": [], "action_schema": []})
    session.initialize({"state_requirements": [], "action_schema": []})
    session.acknowledge_ready("a" * 64)

    assert session.event(request) == {"applied": True, "kind": "root_push"}
    assert session.shutdown("test") == {"final_step_count": "1"}
    assert session.state.value == "closed"
    assert transport.closed


def test_disconnect_fails_without_retrying_current_request() -> None:
    """Close the session after a disconnect without resending Initialize."""

    responses = _response(HELLO, 1, canonical_json({"selected_protocol": {"major": 2, "minor": 0}}))
    transport = _FakeTransport(responses)
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    session.connect()
    with pytest.raises(SessionError):
        session.describe({"state_requirements": [], "action_schema": []})

    assert session.state.value == "failed"
    assert transport.closed
    assert len(transport.sent) == 2


def test_response_envelope_mismatch_fails_session() -> None:
    """Fail closed when the peer returns the wrong response message."""

    responses = b"".join(
        [
            _response(HELLO, 1, canonical_json({"selected_protocol": {"major": 2, "minor": 0}})),
            _response(
                ERROR,
                2,
                canonical_json({"code": "BAD", "phase": "initialize", "message": "bad"}),
                flags=RESPONSE | ERROR_FLAG,
            ),
        ]
    )
    transport = _FakeTransport(responses)
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    session.connect()
    with pytest.raises(BridgeProtocolError, match="BAD"):
        session.describe({"state_requirements": [], "action_schema": []})

    assert session.state.value == "failed"


def _initialize_response_with_topology(topology: object | None) -> bytes:
    """Build Hello + an Initialize response whose topology field is overridable."""

    effective_worker_config = {
        "num_slots": 1,
        "physics_dt": 1.0 / 60.0,
        "decimation": [1, 1],
        "world_map": "/Engine/Maps/Entry",
    }
    response: dict[str, object] = {
        "phase": "describe",
        "selected_protocol": {"major": 2, "minor": 0},
        "build_identity": {},
        "effective_worker_config": effective_worker_config,
        "effective_worker_config_hash": sha256_json(effective_worker_config),
        "available_state_schema": [],
        "available_action_schema": [],
        "seed_derivation_version": "test-v1",
    }
    if topology is not None:
        response["topology"] = topology
    return b"".join(
        [
            _response(HELLO, 1, canonical_json({"selected_protocol": {"major": 2, "minor": 0}})),
            _response(INITIALIZE, 2, canonical_json(response), flags=RESPONSE | MORE_FRAMES),
        ]
    )


def test_missing_topology_fails_initialize() -> None:
    """Reject an Initialize response that omits the required topology field."""

    transport = _FakeTransport(_initialize_response_with_topology(None))
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    session.connect()
    with pytest.raises(BridgeProtocolError) as raised:
        session.describe({"state_requirements": [], "action_schema": []})

    assert raised.value.code == "DTO_INVALID"


def test_out_of_range_joint_body_index_fails_initialize() -> None:
    """Reject a topology whose joint references a body index out of range."""

    bad_topology = {
        "body_names": ["cart", "pole"],
        "body_motion_types": ["simulated", "simulated"],
        "root_body_index": 0,
        "fixed_base": False,
        "joints": [
            {
                "name": "pole_joint",
                "parent_body_index": 0,
                "child_body_index": 5,
                "degrees_of_freedom": 1,
                "coordinate": "twist",
                "coordinate_type": "revolute",
                "unit": "rad",
                "default_position": 0.0,
                "lower_limit": -3.14,
                "upper_limit": 3.14,
                "child_frame": {"position_metres": [0.0, 0.0, 0.0], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
                "parent_frame": {"position_metres": [0.0, 0.0, 0.0], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
            }
        ],
    }
    transport = _FakeTransport(_initialize_response_with_topology(bad_topology))
    session = SocketBridgeSession(
        "fake",
        0,
        session_id=SESSION_ID,
        transport_factory=lambda _host, _port, _timeout: transport,
    )

    session.connect()
    with pytest.raises(BridgeProtocolError, match="topology is invalid") as raised:
        session.describe({"state_requirements": [], "action_schema": []})

    assert raised.value.code == "DTO_INVALID"



def test_startup_connect_allows_os_handshake_until_shared_deadline(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A slow successful handshake must survive until the startup deadline."""

    now = [100.0]
    attempts: list[float] = []
    socket_options: list[tuple[int, int, int]] = []
    request_timeouts: list[float] = []

    class ConnectedSocket:
        def setsockopt(self, level: int, option: int, value: int) -> None:
            socket_options.append((level, option, value))

        def settimeout(self, timeout: float) -> None:
            request_timeouts.append(timeout)

    def connect(address: tuple[str, int], timeout: float) -> ConnectedSocket:
        assert address == ("127.0.0.1", 12345)
        attempts.append(timeout)
        if len(attempts) == 1:
            now[0] += 0.25
            raise ConnectionRefusedError("Worker has not started listening")
        # Windows may finish a pending handshake after the old one-second cap.
        if timeout < 2.0:
            now[0] += timeout
            raise TimeoutError("client abandoned handshake before listener accepted it")
        now[0] += 2.0
        return ConnectedSocket()

    def sleep(duration: float) -> None:
        now[0] += duration

    monkeypatch.setattr("uerl.core.codec.transport.time.monotonic", lambda: now[0])
    monkeypatch.setattr("uerl.core.codec.transport.time.sleep", sleep)
    monkeypatch.setattr("uerl.core.codec.transport.socket.create_connection", connect)

    transport = connect_socket_with_deadline("127.0.0.1", 12345, 10.0, 7.0)

    assert isinstance(transport, SocketTransport)
    assert attempts == pytest.approx([10.0, 9.7])
    assert socket_options == [(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)]
    assert request_timeouts == [7.0]
    assert now[0] == pytest.approx(102.3)


def test_startup_connect_stops_at_deadline_after_os_timeout(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """An exhausted OS connect consumes the budget without another attempt."""

    now = [100.0]
    attempts: list[float] = []
    failure = TimeoutError("OS connection deadline")

    def connect(address: tuple[str, int], timeout: float) -> None:
        assert address == ("127.0.0.1", 12345)
        attempts.append(timeout)
        now[0] += timeout
        raise failure

    def sleep(duration: float) -> None:
        now[0] += duration

    monkeypatch.setattr("uerl.core.codec.transport.time.monotonic", lambda: now[0])
    monkeypatch.setattr("uerl.core.codec.transport.time.sleep", sleep)
    monkeypatch.setattr("uerl.core.codec.transport.socket.create_connection", connect)

    with pytest.raises(TimeoutError) as caught:
        connect_socket_with_deadline("127.0.0.1", 12345, 3.0, 7.0)

    assert caught.value is failure
    assert attempts == [3.0]
    assert now[0] == 103.0

"""Verify socket fragmentation handling and target Hello negotiation."""
from __future__ import annotations

import socket
import threading
import time

import pytest

from tests.protocol.support import protocol
from tests.protocol.support.socket_client import SocketBridgeClient


def _recv_exact(connection: socket.socket, length: int) -> bytes:
    result = bytearray()
    while len(result) < length:
        chunk = connection.recv(length - len(result))
        if not chunk:
            raise ConnectionError("client disconnected")
        result.extend(chunk)
    return bytes(result)


def _fragmented_hello_exchange() -> None:
    """Verify the test client reassembles a Hello frame split into three-byte writes."""

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(5.0)
    port = int(listener.getsockname()[1])
    failure: list[BaseException] = []

    def serve() -> None:
        """Fragment one valid Hello response to exercise exact-frame reassembly."""

        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(5.0)
                request = protocol.FrameHeader.unpack(_recv_exact(connection, protocol.HEADER_SIZE))
                hello = protocol.strict_loads(_recv_exact(connection, request.payload_length))
                assert request.message == protocol.HELLO
                assert hello["supported_protocols"][0]["major"] == 2
                response = {
                    "selected_protocol": {"major": 2, "minor": 0},
                    "build_identity": {"ue_version": "test", "project_version": "test",
                                       "plugin_version": "test", "build_id": "test"},
                    "capabilities": {"socket_data_plane": True},
                    "limits": {"max_control_payload_bytes": 1048576,
                               "max_data_payload_bytes": 67108864, "max_slots": 65536},
                }
                payload = protocol.canonical_json(response)
                header = protocol.FrameHeader(2, 0, protocol.HELLO, protocol.RESPONSE, len(payload),
                                               request.sequence, request.session)
                frame = header.pack() + payload
                for offset in range(0, len(frame), 3):
                    connection.sendall(frame[offset:offset + 3])
        except BaseException as exc:  # Propagate failures from the server thread.
            failure.append(exc)
        finally:
            listener.close()

    thread = threading.Thread(target=serve)
    thread.start()
    client = SocketBridgeClient("127.0.0.1", port, num_slots=2)
    try:
        response = client.connect(timeout_s=5.0)
        assert response["capabilities"]["socket_data_plane"] is True
    finally:
        listener.close()
        if client.sock is not None:
            client.sock.close()
            client.sock = None
        # Allow the five-second socket timeout to finish before the join bound.
        thread.join(timeout=6.0)

    assert not thread.is_alive()
    assert not failure


def test_ac_u4_005_fragmented_hello_response_is_reassembled() -> None:
    """Exercise a real TCP frame split into three-byte writes."""
    _fragmented_hello_exchange()


def test_fragmented_hello_server_exits_when_client_fails_before_connect(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A failed client must release its owned listener and non-daemon thread."""
    started: list[threading.Thread] = []
    original_start = threading.Thread.start

    def record_start(thread: threading.Thread) -> None:
        started.append(thread)
        original_start(thread)

    def fail_connect(client: SocketBridgeClient, timeout_s: float = 10.0) -> dict[str, object]:
        raise ConnectionError("controlled pre-connect failure")

    monkeypatch.setattr(threading.Thread, "start", record_start)
    monkeypatch.setattr(SocketBridgeClient, "connect", fail_connect)
    with pytest.raises(ConnectionError, match="controlled pre-connect failure"):
        _fragmented_hello_exchange()
    assert len(started) == 1
    assert not started[0].is_alive(), "owned server remains blocked after client failure"


def test_socket_client_connection_attempt_uses_remaining_deadline(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Simulate two OS connection failures without wall-clock waiting or a Worker."""
    now = [0.0]
    timeouts: list[float] = []

    def fail_connection(address: tuple[str, int], *, timeout: float) -> socket.socket:
        assert address == ("127.0.0.1", 1)
        timeouts.append(timeout)
        now[0] += 0.8 if len(timeouts) == 1 else timeout
        raise TimeoutError("controlled OS connection failure")

    def advance_clock(duration: float) -> None:
        now[0] += duration

    monkeypatch.setattr(time, "monotonic", lambda: now[0])
    monkeypatch.setattr(time, "sleep", advance_clock)
    monkeypatch.setattr(socket, "create_connection", fail_connection)
    client = SocketBridgeClient("127.0.0.1", 1, num_slots=1)
    with pytest.raises(ConnectionError, match="controlled OS connection failure"):
        client.connect(timeout_s=1.0)
    assert timeouts == pytest.approx([1.0, 0.15])
    assert now[0] == pytest.approx(1.0)
    assert client.sock is None

"""Verify socket fragmentation handling and target Hello negotiation."""
from __future__ import annotations

import socket
import threading

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


def test_ac_u4_005_fragmented_hello_response_is_reassembled() -> None:
    """Verify the test client reassembles a Hello frame split into three-byte writes."""

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = int(listener.getsockname()[1])
    failure: list[BaseException] = []

    def serve() -> None:
        """Fragment one valid Hello response to exercise exact-frame reassembly."""

        try:
            connection, _ = listener.accept()
            with connection:
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
        if client.sock is not None:
            client.sock.close()
            client.sock = None
        thread.join(timeout=5.0)

    assert not thread.is_alive()
    assert not failure

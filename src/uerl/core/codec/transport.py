"""Provide the byte-transport layer for the synchronous U4 Bridge session.

This module owns the transport abstraction and its TCP implementation. It has no
dependency on the Bridge protocol state machine; the session drives it through
the public ``Transport`` surface only.
"""

from __future__ import annotations

import socket
import time
from typing import Protocol


class Transport(Protocol):
    """Provide the byte operations required by the synchronous client.

    Implementations must preserve byte order and surface ambiguous transport
    failures to the owning session; the session does not retry an in-flight
    request.
    """

    def sendall(self, data: bytes) -> None:
        """Send all bytes or raise an OS transport error.

        Args:
            data: Complete frame bytes, including header and payload, to send.
        """

    def recv(self, length: int) -> bytes:
        """Receive up to ``length`` bytes.

        Args:
            length: Maximum number of bytes requested for the next fragment.

        Returns:
            Zero bytes on clean peer disconnect or a non-empty fragment that the
            session assembles until the requested frame portion is complete.
        """

    def close(self) -> None:
        """Close the underlying connection.

        Side effects:
            Release the transport resource. This operation must be safe during
            failure cleanup and must not reconnect.
        """


class BridgeTransportFactory(Protocol):
    """Create a connected transport for the session."""

    def __call__(self, host: str, port: int, timeout_s: float) -> Transport:
        """Open one external SocketBridge connection.

        Args:
            host: Worker endpoint host.
            port: Worker endpoint port.
            timeout_s: Connection deadline in seconds.

        Returns:
            A connected transport ready for the Hello exchange.
        """


class SocketTransport:
    """Adapt one TCP socket to the Bridge transport protocol."""

    def __init__(self, sock: socket.socket) -> None:
        """Wrap an already-created connected socket.

        Args:
            sock: Connected TCP socket owned by this transport.
        """

        self._socket = sock

    @classmethod
    def connect(
        cls,
        host: str,
        port: int,
        timeout_s: float,
        *,
        request_timeout_s: float | None = None,
    ) -> SocketTransport:
        """Open a TCP connection and apply the request deadline after connect.

        Args:
            host: Worker endpoint host.
            port: Worker endpoint port.
            timeout_s: Connection timeout used by ``create_connection``.
            request_timeout_s: Optional per-exchange socket timeout replacing
                the connection timeout after connect.

        Returns:
            A TCP transport with Nagle disabled and a request timeout applied.

        Raises:
            OSError: If the socket cannot connect or configure.
        """

        sock = socket.create_connection((host, port), timeout=timeout_s)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(timeout_s if request_timeout_s is None else request_timeout_s)
        return cls(sock)

    def sendall(self, data: bytes) -> None:
        """Send all bytes through the socket.

        Args:
            data: Complete serialized frame bytes.
        """

        self._socket.sendall(data)

    def recv(self, length: int) -> bytes:
        """Receive bytes from the socket.

        Args:
            length: Maximum fragment size requested from the socket.

        Returns:
            Up to ``length`` bytes; an empty result indicates peer disconnect.
        """

        return self._socket.recv(length)

    def close(self) -> None:
        """Close the socket and release its OS resource."""

        self._socket.close()

    def set_deadline(self, deadline: float) -> None:
        """Apply the remaining absolute request deadline to the socket.

        Args:
            deadline: Absolute ``time.monotonic`` deadline shared by the whole
                request, including fragmented send/receive operations.
        """

        self._socket.settimeout(max(0.0, deadline - time.monotonic()))


def connect_socket_with_deadline(
    host: str,
    port: int,
    connect_timeout_s: float,
    request_timeout_s: float,
) -> SocketTransport:
    """Wait for an externally launched Worker before the Hello exchange.

    The bounded connection attempts cover Worker startup only; once a socket is
    connected, request failures are not retried because request ownership is
    ambiguous. Each OS handshake uses the remaining startup budget; a shorter
    attempt timeout can abandon a connection that the Windows listener accepts.
    """

    deadline = time.monotonic() + connect_timeout_s
    last_error: OSError | None = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if last_error is not None:
                raise last_error
            raise TimeoutError("SocketBridge connection deadline expired")
        try:
            return SocketTransport.connect(
                host,
                port,
                remaining,
                request_timeout_s=request_timeout_s,
            )
        except OSError as exc:
            last_error = exc
            # Retry only connection establishment until the single startup
            # deadline; never replay a partially sent protocol request.
            time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))

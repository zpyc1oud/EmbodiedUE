"""Define the U4 SocketBridge frame header and its wire constants."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from uuid import UUID

from ...errors import BridgeProtocolError

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
EVENT = 0x0103

REQUEST = 0x0001
RESPONSE = 0x0002
ERROR_FLAG = 0x0004
MORE_FRAMES = 0x0008
_KNOWN_FLAGS = REQUEST | RESPONSE | ERROR_FLAG | MORE_FRAMES
CONTROL_MESSAGES = frozenset({HELLO, INITIALIZE, READY, SHUTDOWN, ERROR, EVENT})


@dataclass(frozen=True, slots=True)
class FrameHeader:
    """Represent one U4 48-byte frame header.

    Attributes:
        major: Protocol major version encoded in the frame.
        minor: Protocol minor version encoded in the frame.
        message: Message kind such as Hello, Step, Reset, or Error.
        flags: Exactly one request/response direction plus optional error or
            continuation flags.
        payload_length: Number of bytes following this header.
        sequence: Request/response correlation sequence.
        session: UUID identifying the Bridge session.
        layout_id: Binary layout identity for data-plane frames, or zero for
            control frames.
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
        """Pack this typed header into its wire representation.

        Returns:
            Exactly 48 big-endian header bytes. Field validity is checked when
            the peer or ``unpack`` validates the resulting envelope.
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
        """Decode and boundary-check one received 48-byte header.

        Args:
            data: Exactly one received 48-byte frame header.

        Returns:
            A typed header after magic, flags, message, and payload-limit checks.

        Raises:
            BridgeProtocolError: If the size, magic, direction/error flags, or
                payload length violates the frozen frame contract.
        """

        if len(data) != HEADER_SIZE:
            raise BridgeProtocolError("header must be exactly 48 bytes", code="HEADER_SIZE")
        magic, major, minor, message, flags, length, sequence, session, layout_id = HEADER.unpack(data)
        if magic != MAGIC:
            raise BridgeProtocolError("frame magic mismatch", code="MAGIC_MISMATCH")
        if flags & ~_KNOWN_FLAGS:
            raise BridgeProtocolError("unknown frame flags", code="FLAGS_UNKNOWN")
        if (flags & (REQUEST | RESPONSE)) not in (REQUEST, RESPONSE):
            raise BridgeProtocolError("exactly one direction flag is required", code="FLAGS_DIRECTION")
        if message == ERROR and (flags & (RESPONSE | ERROR_FLAG)) != (RESPONSE | ERROR_FLAG):
            raise BridgeProtocolError("Error frame must be an Error response", code="FLAGS_ERROR_FRAME")
        if message != ERROR and flags & ERROR_FLAG:
            raise BridgeProtocolError("only Error frames may set the error flag", code="FLAGS_ERROR_FLAG")
        if length > MAX_DATA_PAYLOAD_BYTES:
            raise BridgeProtocolError("payload exceeds the data limit", code="PAYLOAD_TOO_LARGE")
        return cls(major, minor, message, flags, length, sequence, UUID(bytes=session), layout_id)

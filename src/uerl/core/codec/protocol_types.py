"""Typed U4 Bridge lifecycle enums, initialization results, and protocol interface."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from enum import StrEnum
from typing import Any, Protocol, cast

from ..config.robot import RobotTopology
from .frames import FrameHeader


class BridgeState(StrEnum):
    """Describe the externally visible lifecycle state of a Bridge session."""

    DISCONNECTED = "disconnected"
    CONNECTED = "connected"
    INITIALIZED = "initialized"
    READY = "ready"
    FAILED = "failed"
    CLOSED = "closed"


@dataclass(frozen=True, slots=True)
class InitializeResult:
    """Return the JSON negotiation and optional InitialState frame."""

    response: dict[str, Any]
    initial_header: FrameHeader
    initial_payload: bytes

    @property
    def topology(self) -> RobotTopology:
        """Return the validated initialize topology as a typed dataclass."""

        return RobotTopology.from_response(cast(Mapping[str, object], self.response["topology"]))


@dataclass(frozen=True, slots=True)
class InitializeDescription:
    """Return the reflected Worker contract before Task schema commit."""

    response: dict[str, Any]

    @property
    def topology(self) -> RobotTopology:
        """Return the validated topology advertised by the Worker."""

        return RobotTopology.from_response(cast(Mapping[str, object], self.response["topology"]))


class IBridgeSession(Protocol):
    """Define the synchronous, single-request protocol surface consumed by UERLSession.

    Calls are lifecycle-ordered and only one request may be in flight. A timeout,
    protocol error, or ambiguous transport failure invalidates the session; the
    implementation must not retry or resend that request.
    """

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest successful Step."""

        ...

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest successful Reset."""

        ...

    def connect(self) -> dict[str, Any]:
        """Connect and complete Hello before any Initialize request.

        Returns:
            The negotiated Hello response DTO.

        Raises:
            SessionError: If connection or the Hello exchange fails.
            BridgeProtocolError: If the response violates the protocol.
        """

        ...

    def initialize(self, request: dict[str, Any]) -> InitializeResult:
        """Initialize the Worker and receive its InitialState frame before Ready.

        Args:
            request: Typed Worker projection and Task schema request.

        Returns:
            The validated Initialize DTO plus the first binary InitialState frame.

        Raises:
            SessionError: If transport or lifecycle exchange fails.
            BridgeProtocolError: If the DTO, layouts, or InitialState frame is
                invalid.
        """

        ...

    def describe(self, request: dict[str, Any]) -> InitializeDescription:
        """Describe the reflected Worker contract without committing layouts."""

        ...

    def acknowledge_ready(self, manifest_hash: str) -> dict[str, Any]:
        """Persist the Manifest hash and enter the Ready state.

        Args:
            manifest_hash: Canonical audit identity of the persisted Manifest.

        Returns:
            The accepted ``{"ready": True}`` response.

        Raises:
            SessionError: If the session is not initialized or the exchange fails.
            BridgeProtocolError: If the Worker rejects the identity or DTO.
        """

        ...

    def step(self, layout_id: int, payload: bytes) -> tuple[FrameHeader, bytes]:
        """Exchange one Step payload while the session is Ready.

        Args:
            layout_id: Negotiated Step-action layout identity.
            payload: Exact binary batch for that layout.

        Returns:
            The validated response header and Step-result payload.
        """

        ...

    def reset(self, layout_id: int, payload: bytes) -> tuple[FrameHeader, bytes]:
        """Exchange one Reset payload while the session is Ready.

        Args:
            layout_id: Negotiated Reset-request layout identity.
            payload: Exact binary reset-mask batch for that layout.

        Returns:
            The validated response header and Reset-result payload.
        """

        ...

    def event(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """Exchange one JSON event request while the session is Ready."""

        ...

    def shutdown(self, reason: str) -> dict[str, Any]:
        """Shutdown the Worker and close the connection without retrying.

        Args:
            reason: Audit reason sent in the Shutdown DTO.

        Returns:
            The Worker shutdown acknowledgement.

        Side effects:
            Request Worker shutdown and release the transport even when the
            acknowledgement fails.
        """

        ...

    def close(self) -> None:
        """Close the transport without retrying, reconnecting, or resending.

        Side effects:
            Release the current transport and make further lifecycle exchanges
            invalid until a new session object is created.
        """

        ...

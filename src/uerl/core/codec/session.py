"""Implement the synchronous, single-request-in-flight U4 Bridge session."""

from __future__ import annotations

import math
import threading
import time
from collections.abc import Mapping
from dataclasses import dataclass
from enum import StrEnum
from typing import Any, NoReturn, Protocol, cast
from uuid import UUID, uuid4

from ...errors import BridgeProtocolError, SessionError
from ..config.models import DECIMATION_INT32_MAX, DECIMATION_INT32_MIN
from ..config.robot import RobotTopology
from .batch import BatchCodec, Layout
from .frames import (
    CONTROL_MESSAGES,
    ERROR,
    EVENT,
    HEADER_SIZE,
    HELLO,
    INITIAL_STATE,
    INITIALIZE,
    MAX_DATA_PAYLOAD_BYTES,
    MORE_FRAMES,
    PROTOCOL,
    READY,
    REQUEST,
    RESET,
    RESPONSE,
    SHUTDOWN,
    STEP,
    FrameHeader,
)
from .json import MAX_CONTROL_PAYLOAD_BYTES, canonical_json, sha256_json, strict_loads
from .transport import (
    BridgeTransportFactory,
    SocketTransport,
    Transport,
    connect_socket_with_deadline,
)


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


class SocketBridgeSession:
    """Run one synchronous U4 SocketBridge connection.

    Permit only one request in flight, consume one absolute deadline per
    exchange, and transition to failure on protocol, timeout, or transport
    ambiguity instead of retrying the request.
    """

    def __init__(
        self,
        host: str,
        port: int,
        *,
        client_name: str = "uerl-python",
        client_version: str = "0.1.0",
        session_id: UUID | None = None,
        transport_factory: BridgeTransportFactory | None = None,
        connect_timeout_s: float = 10.0,
        request_timeout_s: float = 10.0,
        protocol_range: tuple[int, int, int] = (PROTOCOL[0], 0, PROTOCOL[1]),
    ) -> None:
        """Configure one single-request-in-flight Bridge session.

        Args:
            host: Worker endpoint host.
            port: Worker endpoint port.
            client_name: Client identity sent during Hello.
            client_version: Client version sent during Hello.
            session_id: Optional stable UUID; a new UUID is generated when
                omitted.
            transport_factory: Optional connected transport factory.
            connect_timeout_s: Total deadline for connecting and completing the
                external Worker availability wait.
            request_timeout_s: Absolute deadline budget for each exchange.
            protocol_range: ``(major, min_minor, max_minor)`` requested during
                Hello.

        Side effects:
            None until ``connect``. This object owns the transport after
            connection and closes it on protocol, timeout, or transport failure.
        """

        self._host = host
        self._port = port
        self._client_name = client_name
        self._client_version = client_version
        self._session_id = session_id or uuid4()
        self._transport_factory = transport_factory or (
            lambda host, port, timeout_s: connect_socket_with_deadline(
                host,
                port,
                timeout_s,
                request_timeout_s,
            )
        )
        self._connect_timeout_s = connect_timeout_s
        self._request_timeout_s = request_timeout_s
        self._protocol_range = protocol_range
        self._selected_protocol = PROTOCOL
        self._transport: Transport | None = None
        self._state = BridgeState.DISCONNECTED
        self._sequence = 0
        self._in_flight = 0
        self._in_flight_max = 0
        self._layout_ids: dict[str, int] = {}
        self._layouts: dict[str, Layout] = {}
        self._pending_description: InitializeDescription | None = None
        self._exchange_lock = threading.Lock()
        self._last_exchange_deadline: float | None = None
        self._last_exchange_timing: dict[str, float] = {}
        self._last_step_timing: dict[str, float] = {}
        self._last_reset_timing: dict[str, float] = {}

    @property
    def state(self) -> BridgeState:
        """Return the current session state.

        Returns:
            The lifecycle state used to gate public exchanges. FAILED is retained
            after cleanup so callers can distinguish an invalidated session from
            an intentional close.
        """

        return self._state

    @property
    def in_flight_max(self) -> int:
        """Return the largest number of simultaneous exchanges observed.

        Returns:
            The maximum number of concurrent calls admitted by the exchange lock;
            a correct synchronous client observes one.
        """

        return self._in_flight_max

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest successful Step."""

        return self._last_step_timing

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest successful Reset."""

        return self._last_reset_timing

    def connect(self) -> dict[str, Any]:
        """Open the transport and complete the bootstrap Hello exchange.

        Returns:
            The validated Hello negotiation response, including selected protocol
            and Worker capabilities.

        Raises:
            SessionError: If transport creation or connection fails.
            BridgeProtocolError: If the Hello envelope, selected version, or
                response DTO is invalid. The transport is closed on failure.

        Side effects:
            Move the session from DISCONNECTED to CONNECTED and retain the
            selected protocol. This operation is valid only once.
        """

        self._ensure_state(BridgeState.DISCONNECTED)
        try:
            self._transport = self._transport_factory(self._host, self._port, self._connect_timeout_s)
        except (OSError, TimeoutError) as exc:
            self._state = BridgeState.FAILED
            raise SessionError("SocketBridge connection failed") from exc
        payload = {
            "supported_protocols": [{
                "major": self._protocol_range[0],
                "min_minor": self._protocol_range[1],
                "max_minor": self._protocol_range[2],
            }],
            "client_name": self._client_name,
            "client_version": self._client_version,
        }
        try:
            header, response_payload = self._exchange(HELLO, 0, canonical_json(payload), bootstrap=True)
            response = strict_loads(response_payload)
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            self.close()
            raise
        selected = response.get("selected_protocol")
        if not isinstance(selected, dict) or selected.get("major") != self._protocol_range[0]:
            self._raise_protocol("Hello selected an unsupported protocol", code="VERSION_UNSUPPORTED")
        selected_minor = selected.get("minor")
        if (
            not isinstance(selected_minor, int)
            or not self._protocol_range[1] <= selected_minor <= self._protocol_range[2]
        ):
            self._raise_protocol("Hello selected an unsupported protocol", code="VERSION_UNSUPPORTED")
        if (header.major, header.minor) != (self._protocol_range[0], selected_minor):
            self._raise_protocol("Hello response version does not match its payload", code="VERSION_UNSUPPORTED")
        self._selected_protocol = (self._protocol_range[0], selected_minor)
        self._state = BridgeState.CONNECTED
        return response

    def initialize(self, request: dict[str, Any]) -> InitializeResult:
        """Initialize the Worker and receive its InitialState frame.

        Args:
            request: JSON-compatible Worker projection and Task schema request.
                It is serialized once for the Initialize exchange.

        Returns:
            The validated Initialize response and the following InitialState
            frame, with all negotiated layouts retained by this session.

        Raises:
            SessionError: If the transport, timeout, or lifecycle exchange fails.
            BridgeProtocolError: If the DTO, layout identities, or InitialState
                payload violates the protocol. Failure closes the transport.

        Side effects:
            Negotiate and cache all required layouts, validate the first state
            batch, and move from CONNECTED to INITIALIZED.
        """

        if self._state is not BridgeState.CONNECTED:
            self._raise_protocol(
                f"Initialize is not valid in state {self._state.value}",
                code="STATE_VIOLATION",
                phase="initialize",
            )
        if self._pending_description is None:
            self._raise_protocol(
                "Initialize commit requires a preceding Describe",
                code="STATE_VIOLATION",
                phase="initialize",
            )
        commit_request = dict(request)
        commit_request["phase"] = "commit"
        try:
            header, response_payload = self._exchange(INITIALIZE, 0, canonical_json(commit_request))
            response = strict_loads(response_payload)
            self._validate_initialize_response(response)
            if not header.flags & MORE_FRAMES:
                self._raise_protocol("Initialize did not announce InitialState", code="INITIAL_STATE_MISSING")
            try:
                batch_size = response["effective_worker_config"]["num_slots"]
                layouts = tuple(
                    Layout.parse(descriptor, batch_size, protocol_version=self._selected_protocol)
                    for descriptor in response["layouts"]
                )
                self._layouts = {layout.kind: layout for layout in layouts}
                self._layout_ids = {kind: layout.layout_id for kind, layout in self._layouts.items()}
                if set(self._layout_ids) != {
                    "initial_state",
                    "step_action",
                    "step_result",
                    "reset_request",
                    "reset_result",
                }:
                    raise self._protocol_failure(
                        "Initialize response omitted a required layout",
                        code="LAYOUT_NEGOTIATION",
                    )
            except (KeyError, TypeError, ValueError) as exc:
                raise self._protocol_failure(
                    "Initialize response omitted valid layout descriptors",
                    code="LAYOUT_NEGOTIATION",
                ) from exc
            initial_header, initial_payload = self._recv_frame(self._last_exchange_deadline)
            self._validate_response(
                initial_header,
                INITIAL_STATE,
                header.sequence,
                self._layout_ids["initial_state"],
            )
            BatchCodec.validate_and_map(self._layouts["initial_state"], initial_payload)
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            self.close()
            raise
        except (OSError, TimeoutError) as exc:
            self._state = BridgeState.FAILED
            self.close()
            raise SessionError("SocketBridge InitialState exchange failed") from exc
        self._state = BridgeState.INITIALIZED
        self._pending_description = None
        return InitializeResult(response, initial_header, initial_payload)

    def describe(self, request: dict[str, Any]) -> InitializeDescription:
        """Run the first, non-committing Initialize phase."""

        if self._state is not BridgeState.CONNECTED:
            self._raise_protocol(
                f"Describe is not valid in state {self._state.value}",
                code="STATE_VIOLATION",
                phase="initialize",
            )
        if self._pending_description is not None:
            self._raise_protocol(
                "Initialize describe was already completed",
                code="STATE_VIOLATION",
                phase="initialize",
            )
        described = dict(request)
        described["phase"] = "describe"
        try:
            header, response_payload = self._exchange(INITIALIZE, 0, canonical_json(described))
            response = strict_loads(response_payload)
            self._validate_initialize_description(response)
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            self.close()
            raise
        except (OSError, TimeoutError) as exc:
            self._state = BridgeState.FAILED
            self.close()
            raise SessionError("SocketBridge Initialize description failed") from exc
        self._pending_description = InitializeDescription(response)
        return self._pending_description

    def _validate_initialize_description(self, response: dict[str, Any]) -> None:
        """Validate the topology/config DTO returned by Initialize describe."""

        required = {
            "phase",
            "selected_protocol",
            "build_identity",
            "effective_worker_config",
            "effective_worker_config_hash",
            "available_state_schema",
            "available_action_schema",
            "topology",
            "seed_derivation_version",
        }
        if (
            set(response) - required - {"extensions"}
            or not required.issubset(response)
            or response["phase"] != "describe"
        ):
            raise self._protocol_failure("Initialize description contains invalid fields", code="DTO_INVALID")
        if response["selected_protocol"] != {
            "major": self._selected_protocol[0],
            "minor": self._selected_protocol[1],
        }:
            raise self._protocol_failure("Initialize description protocol does not match Hello", code="DTO_INVALID")
        worker_config = response["effective_worker_config"]
        if not isinstance(worker_config, dict) or response["effective_worker_config_hash"] != sha256_json(
            worker_config
        ):
            raise self._protocol_failure("Initialize description worker config hash mismatch", code="DTO_INVALID")
        if not isinstance(response["available_state_schema"], list) or not isinstance(
            response["available_action_schema"], list
        ) or not isinstance(response["topology"], dict):
            raise self._protocol_failure("Initialize description schema types are invalid", code="DTO_INVALID")
        try:
            RobotTopology.from_response(cast(Mapping[str, object], response["topology"]))
        except (TypeError, ValueError) as exc:
            raise self._protocol_failure("Initialize description topology is invalid", code="DTO_INVALID") from exc

    def _validate_initialize_response(self, response: dict[str, Any]) -> None:
        """Validate the required Initialize DTO before exposing it to Session."""

        required = {
            "phase",
            "selected_protocol",
            "build_identity",
            "effective_worker_config",
            "effective_worker_config_hash",
            "available_state_schema",
            "available_action_schema",
            "topology",
            "selected_schemas",
            "layouts",
            "seed_derivation_version",
        }
        if (
            set(response) - required - {"extensions"}
            or not required.issubset(response)
            or response.get("phase") != "commit"
        ):
            raise self._protocol_failure(
                "Initialize commit response contains invalid fields",
                code="DTO_INVALID",
            )
        if response["selected_protocol"] != {
            "major": self._selected_protocol[0],
            "minor": self._selected_protocol[1],
        }:
            raise self._protocol_failure("Initialize protocol does not match Hello", code="DTO_INVALID")
        if not isinstance(response["build_identity"], dict) or not isinstance(response["seed_derivation_version"], str):
            raise self._protocol_failure("Initialize identity fields have invalid types", code="DTO_INVALID")
        worker_config = response["effective_worker_config"]
        if not isinstance(worker_config, dict):
            raise self._protocol_failure("effective_worker_config must be an object", code="DTO_INVALID")
        if response["effective_worker_config_hash"] != sha256_json(worker_config):
            raise self._protocol_failure("effective_worker_config_hash mismatch", code="DTO_INVALID")
        for key in ("num_slots", "physics_dt", "decimation", "world_map"):
            if key not in worker_config:
                raise self._protocol_failure("effective_worker_config is incomplete", code="DTO_INVALID")
        if (
            not isinstance(worker_config["num_slots"], int)
            or isinstance(worker_config["num_slots"], bool)
            or worker_config["num_slots"] < 1
            or not isinstance(worker_config["physics_dt"], (int, float))
            or isinstance(worker_config["physics_dt"], bool)
            or not math.isfinite(worker_config["physics_dt"])
            or worker_config["physics_dt"] <= 0
            or not isinstance(worker_config["decimation"], list)
            or len(worker_config["decimation"]) != 2
            or any(
                type(value) is not int
                or value < DECIMATION_INT32_MIN
                or value > DECIMATION_INT32_MAX
                for value in worker_config["decimation"]
            )
            or worker_config["decimation"][0] > worker_config["decimation"][1]
            or not isinstance(worker_config["world_map"], str)
            or not worker_config["world_map"].startswith("/")
        ):
            raise self._protocol_failure("effective_worker_config has invalid values", code="DTO_INVALID")
        if (
            not isinstance(response["available_state_schema"], list)
            or not isinstance(response["available_action_schema"], list)
        ):
            raise self._protocol_failure("Initialize schemas must be arrays", code="DTO_INVALID")
        self._validate_topology(response["topology"])
        selected_schemas = response["selected_schemas"]
        if not isinstance(selected_schemas, dict) or not all(
            isinstance(selected_schemas.get(key), str) and len(selected_schemas[key]) == 64
            for key in ("state_schema_hash", "action_schema_hash")
        ):
            raise self._protocol_failure("Initialize selected schema hashes are invalid", code="DTO_INVALID")
        if not isinstance(response["layouts"], list):
            raise self._protocol_failure("Initialize layouts must be an array", code="DTO_INVALID")

    def _validate_topology(self, topology: Any) -> None:
        """Validate the reflected topology once at the protocol boundary."""

        if not isinstance(topology, Mapping):
            raise self._protocol_failure("Initialize topology is invalid", code="DTO_INVALID")
        try:
            RobotTopology.from_response(topology)
        except (KeyError, TypeError, ValueError) as exc:
            raise self._protocol_failure("Initialize topology is invalid", code="DTO_INVALID") from exc

    def acknowledge_ready(self, manifest_hash: str) -> dict[str, Any]:
        """Send the persisted Manifest hash and enter Ready.

        Args:
            manifest_hash: Canonical hash of the Manifest persisted before this
                call. It is sent as the Worker audit identity.

        Returns:
            The accepted Ready response.

        Raises:
            SessionError: If the session is not INITIALIZED or the exchange fails.
            BridgeProtocolError: If the Worker rejects the hash or response DTO.

        Side effects:
            Move the session to READY. Protocol failure closes the transport and
            leaves the state FAILED.
        """

        if self._state is not BridgeState.INITIALIZED:
            self._state = BridgeState.FAILED
            self.close()
            raise SessionError(f"Ready is not valid in state {self._state.value}")
        try:
            header, response_payload = self._exchange(
                READY,
                0,
                canonical_json({"manifest_hash": manifest_hash}),
            )
            response = strict_loads(response_payload)
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            self.close()
            raise
        if response != {"ready": True}:
            self._raise_protocol("Ready acknowledgement was not accepted", code="READY_REJECTED")
        self._state = BridgeState.READY
        return response

    def step(self, layout_id: int, payload: bytes) -> tuple[FrameHeader, bytes]:
        """Exchange one Step payload while the session is Ready.

        Args:
            layout_id: Exact negotiated ``step_action`` layout identity.
            payload: Binary command batch whose length, padding, and finite
                values match that layout.

        Returns:
            The response header and validated ``step_result`` payload.

        Raises:
            SessionError: If the state, single-request rule, or transport
                deadline is invalid.
            BridgeProtocolError: If request or response layout/payload checks
                fail; the session is then FAILED and closed.
        """

        self._ensure_state(BridgeState.READY)
        return self._exchange_batch("step_action", "step_result", STEP, layout_id, payload)

    def reset(self, layout_id: int, payload: bytes) -> tuple[FrameHeader, bytes]:
        """Exchange one Reset payload while the session is Ready.

        Args:
            layout_id: Exact negotiated ``reset_request`` layout identity.
            payload: Binary reset-mask batch whose padding and length match the
                negotiated layout.

        Returns:
            The response header and validated ``reset_result`` payload.

        Raises:
            SessionError: If the state, single-request rule, or transport
                deadline is invalid.
            BridgeProtocolError: If request or response layout/payload checks
                fail; the session is then FAILED and closed.
        """

        self._ensure_state(BridgeState.READY)
        return self._exchange_batch("reset_request", "reset_result", RESET, layout_id, payload)

    def event(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """Apply one Session-mediated event mutation to selected Slots."""

        self._ensure_state(BridgeState.READY)
        try:
            _header, response_payload = self._exchange(EVENT, 0, canonical_json(dict(request)))
            response = strict_loads(response_payload)
            if not isinstance(response, dict):
                raise self._protocol_failure("Event response must be an object", code="DTO_INVALID")
            return response
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            self.close()
            raise
        except (OSError, TimeoutError) as exc:
            self._state = BridgeState.FAILED
            self.close()
            raise SessionError("SocketBridge Event exchange failed") from exc

    def _exchange_batch(
        self,
        request_kind: str,
        response_kind: str,
        message: int,
        layout_id: int,
        payload: bytes,
    ) -> tuple[FrameHeader, bytes]:
        request_layout = self._layouts[request_kind]
        if layout_id != request_layout.layout_id:
            self._raise_protocol("request layout mismatch", code="REQUEST_LAYOUT")
        try:
            validate_start = time.perf_counter()
            BatchCodec.validate_and_map(request_layout, payload)
            request_validate_s = time.perf_counter() - validate_start
            header, response_payload = self._exchange(
                message,
                layout_id,
                payload,
                response_layout_id=self._layouts[response_kind].layout_id,
            )
            validate_start = time.perf_counter()
            BatchCodec.validate_and_map(self._layouts[response_kind], response_payload)
            response_validate_s = time.perf_counter() - validate_start
            timings = {
                "client_request_validate": request_validate_s,
                **self._last_exchange_timing,
                "client_response_validate": response_validate_s,
            }
            if message == STEP:
                self._last_step_timing = timings
            elif message == RESET:
                self._last_reset_timing = timings
            return header, response_payload
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            self.close()
            raise

    def shutdown(self, reason: str) -> dict[str, Any]:
        """Send Shutdown and close the connection without retry.

        Args:
            reason: Audit reason included in the Shutdown control DTO.

        Returns:
            The Worker shutdown acknowledgement when the exchange succeeds.

        Raises:
            SessionError: If shutdown is requested before Hello or after close.
            BridgeProtocolError: If the Worker response is malformed. Transport
                cleanup still runs in the ``finally`` path.

        Side effects:
            Request Worker shutdown, close the transport, and move to CLOSED on
            success. No resend or reconnect is attempted.
        """

        self._ensure_state(BridgeState.CONNECTED, BridgeState.INITIALIZED, BridgeState.READY)
        try:
            _header, response_payload = self._exchange(SHUTDOWN, 0, canonical_json({"reason": reason}))
            response = strict_loads(response_payload)
            self._state = BridgeState.CLOSED
            return response
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            raise
        finally:
            self.close()

    def close(self) -> None:
        """Close the current transport; never reconnect or resend a request.

        Side effects:
            Release the current transport and mark a non-FAILED session CLOSED.
            FAILED remains FAILED after cleanup. Repeated calls are idempotent.
        """

        transport, self._transport = self._transport, None
        if transport is not None:
            transport.close()
        if self._state is not BridgeState.FAILED:
            self._state = BridgeState.CLOSED

    def _exchange(
        self,
        message: int,
        layout_id: int,
        payload: bytes,
        *,
        bootstrap: bool = False,
        response_layout_id: int | None = None,
    ) -> tuple[FrameHeader, bytes]:
        if not self._exchange_lock.acquire(blocking=False):
            raise SessionError("Bridge session permits only one request in flight")
        try:
            return self._exchange_locked(
                message,
                layout_id,
                payload,
                bootstrap=bootstrap,
                response_layout_id=response_layout_id,
            )
        finally:
            self._exchange_lock.release()

    def _exchange_locked(
        self,
        message: int,
        layout_id: int,
        payload: bytes,
        *,
        bootstrap: bool = False,
        response_layout_id: int | None = None,
    ) -> tuple[FrameHeader, bytes]:
        transport = self._transport
        if transport is None:
            raise SessionError("Bridge transport is not connected")
        payload_limit = MAX_CONTROL_PAYLOAD_BYTES if message in CONTROL_MESSAGES else MAX_DATA_PAYLOAD_BYTES
        if len(payload) > payload_limit:
            raise self._protocol_failure("request payload exceeds the limit", code="PAYLOAD_TOO_LARGE")
        self._sequence += 1
        sequence = self._sequence
        self._in_flight += 1
        self._in_flight_max = max(self._in_flight_max, self._in_flight)
        try:
            # One absolute deadline covers the complete exchange; refreshing a
            # timeout for each fragment would allow a stalled peer to hang forever.
            deadline = time.monotonic() + self._request_timeout_s
            self._last_exchange_deadline = deadline
            if isinstance(transport, SocketTransport):
                transport.set_deadline(deadline)
            version = (0, 0) if bootstrap else self._selected_protocol
            request = FrameHeader(
                version[0],
                version[1],
                message,
                REQUEST,
                len(payload),
                sequence,
                self._session_id,
                layout_id,
            )
            send_start = time.perf_counter()
            transport.sendall(request.pack() + payload)
            socket_send_s = time.perf_counter() - send_start
            receive_start = time.perf_counter()
            response_header, response_payload = self._recv_frame(deadline)
            response_wait_recv_s = time.perf_counter() - receive_start
            if response_header.message == ERROR:
                self._raise_remote_error(response_payload)
            self._validate_response(
                response_header,
                message,
                sequence,
                layout_id if response_layout_id is None else response_layout_id,
            )
            self._last_exchange_timing = {
                "client_socket_send": socket_send_s,
                "client_response_wait_recv": response_wait_recv_s,
            }
            return response_header, response_payload
        except BridgeProtocolError:
            self._state = BridgeState.FAILED
            self.close()
            raise
        except (OSError, TimeoutError) as exc:
            self._state = BridgeState.FAILED
            self.close()
            raise SessionError("SocketBridge exchange failed") from exc
        finally:
            self._in_flight -= 1

    def _recv_frame(self, deadline: float | None = None) -> tuple[FrameHeader, bytes]:
        transport = self._transport
        if transport is None:
            raise SessionError("Bridge transport is not connected")
        header = FrameHeader.unpack(self._recv_exact(HEADER_SIZE, deadline))
        payload_limit = MAX_CONTROL_PAYLOAD_BYTES if header.message in CONTROL_MESSAGES else MAX_DATA_PAYLOAD_BYTES
        if header.payload_length > payload_limit:
            raise BridgeProtocolError("response payload exceeds the limit", code="PAYLOAD_TOO_LARGE")
        return header, self._recv_exact(header.payload_length, deadline)

    def _recv_exact(self, length: int, deadline: float | None = None) -> bytes:
        transport = self._transport
        if transport is None:
            raise SessionError("Bridge transport is not connected")
        chunks: list[bytes] = []
        remaining = length
        while remaining:
            if deadline is not None and isinstance(transport, SocketTransport):
                transport.set_deadline(deadline)
            # TCP may fragment any frame boundary, so accumulate until the
            # requested header or payload length is complete.
            chunk = transport.recv(remaining)
            if not chunk:
                raise ConnectionError("SocketBridge disconnected during frame")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _validate_response(self, header: FrameHeader, message: int, sequence: int, layout_id: int) -> None:
        if header.flags & RESPONSE == 0 or header.message != message or header.sequence != sequence:
            raise self._protocol_failure("response envelope mismatch", code="RESPONSE_ENVELOPE")
        if header.session != self._session_id or (
            message != HELLO and (header.major, header.minor) != self._selected_protocol
        ):
            raise self._protocol_failure("response protocol/session mismatch", code="RESPONSE_SESSION")
        if header.layout_id != layout_id and message >= 0x100:
            raise self._protocol_failure("response layout mismatch", code="RESPONSE_LAYOUT")

    def _raise_remote_error(self, payload: bytes) -> None:
        error = strict_loads(payload)
        try:
            code = str(error["code"])
            phase = str(error["phase"])
            message = str(error["message"])
        except KeyError as exc:
            raise self._protocol_failure("Malformed ErrorResponse", code="ERROR_RESPONSE") from exc
        raise self._protocol_failure(f"{code} phase={phase}: {message}", code=code, phase=phase)

    def _ensure_state(self, *allowed: BridgeState) -> None:
        if self._state not in allowed:
            raise SessionError(f"Bridge operation is not valid in state {self._state.value}")

    @staticmethod
    def _protocol_failure(message: str, *, code: str, phase: str = "client") -> BridgeProtocolError:
        return BridgeProtocolError(message, code=code, phase=phase)

    def _raise_protocol(self, message: str, *, code: str, phase: str = "client") -> NoReturn:
        self._state = BridgeState.FAILED
        self.close()
        raise self._protocol_failure(message, code=code, phase=phase)

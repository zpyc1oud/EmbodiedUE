"""Provide a test-only client for the production U4 SocketBridge wire protocol."""

from __future__ import annotations

import math
import socket
import time
from typing import Any
from uuid import UUID, uuid4

import numpy as np

from tests.protocol.support import protocol


class SocketBridgeError(RuntimeError):
    """Raise when the test client receives an unexpected Bridge response."""

    pass


class SocketBridgeClient:
    """Drive the production U4 SocketBridge from black-box integration tests.

    The client is intentionally test-only and mirrors the frozen wire protocol;
    it is not a product Session implementation.
    """

    def __init__(
        self,
        host: str,
        port: int,
        num_slots: int,
        seed: int = 0,
        *,
        task_id: str = "uerl.task.cartpole.v1",
        worker_config: dict[str, Any] | None = None,
        action_schema: list[dict[str, Any]] | None = None,
        state_schema: list[dict[str, Any]] | None = None,
    ) -> None:
        """Configure a test client for one Worker endpoint.

        Args:
            host: SocketBridge host.
            port: SocketBridge port.
            num_slots: Negotiated Worker Slot count.
            seed: Worker Run seed used by the scripted Config.
        """

        self.host = host
        self.port = port
        self.num_slots = num_slots
        self.task_id = task_id
        self.worker_config = (
            worker_config if worker_config is not None else protocol.generic_cartpole_worker_config(num_slots, seed)
        )
        configured_decimation = self.worker_config.get("decimation", [1, 1])
        self.step_decimation = (
            configured_decimation[0]
            if isinstance(configured_decimation, list)
            else configured_decimation
        )
        schema = protocol.generic_cartpole_schema()
        self.action_schema = action_schema if action_schema is not None else schema["action_schema"]
        self.state_schema = state_schema if state_schema is not None else schema["state_requirements"]
        self.session = uuid4()
        self.sock: socket.socket | None = None
        self.sequence = 0
        self.layouts: dict[str, protocol.Layout] = {}
        self.hello_response: dict[str, Any] = {}
        self.initialize_response: dict[str, Any] = {}
        self.initial_episode_indices = np.zeros(num_slots, dtype=np.uint64)
        action_width = sum(math.prod(field["shape"]) if field["shape"] else 1 for field in self.action_schema)
        self._actions = np.zeros((num_slots, action_width), dtype=np.float32)

    def connect(self, timeout_s: float = 10.0) -> dict[str, Any]:
        """Connect, complete Hello, and return the negotiated capabilities.

        Args:
            timeout_s: Total connection retry deadline.

        Returns:
            The selected protocol and capability DTO.

        Raises:
            ConnectionError: If no socket connects before the deadline.
            SocketBridgeError: If Hello selects an unexpected protocol.
        """

        deadline = time.monotonic() + timeout_s
        last_error: Exception | None = None
        while (remaining := deadline - time.monotonic()) > 0.0:
            try:
                self.sock = socket.create_connection((self.host, self.port), timeout=remaining)
                self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.sock.settimeout(timeout_s)
                break
            except OSError as exc:
                last_error = exc
                if self.sock is not None:
                    self.sock.close()
                    self.sock = None
                remaining = deadline - time.monotonic()
                if remaining > 0.0:
                    time.sleep(min(0.05, remaining))
        if self.sock is None:
            raise ConnectionError(f"cannot connect to SocketBridge: {last_error}")
        response, _ = self._json_request(
            protocol.HELLO,
            {
                "supported_protocols": [{"major": 2, "min_minor": 0, "max_minor": 0}],
                "client_name": "uerl-test-client",
                "client_version": "0.1.0",
            },
            bootstrap=True,
        )
        if response["selected_protocol"] != {"major": 2, "minor": 0}:
            raise SocketBridgeError("SocketBridge selected an unexpected protocol")
        self.hello_response = response
        return response

    def initialize(self, *, acknowledge_ready: bool = True) -> np.ndarray:
        """Initialize the Worker, optionally acknowledge Ready, and return State.

        Args:
            acknowledge_ready: Whether to send the test Manifest hash and enter
                Ready after InitialState decoding.

        Returns:
            Row-major initial state values for all Slots.

        Raises:
            SocketBridgeError: If negotiation, layout, fault, or Ready checks
                fail.
        """

        request = {
            "task_id": self.task_id,
            "task_version": "1.0.0",
            "worker_config": self.worker_config,
            "worker_config_hash": protocol.sha256(self.worker_config),
            "state_requirements": self.state_schema,
            "action_schema": self.action_schema,
        }
        description, _ = self._json_request(protocol.INITIALIZE, {**request, "phase": "describe"})
        if description.get("phase") != "describe":
            raise SocketBridgeError("Initialize description did not announce describe phase")
        response, header = self._json_request(protocol.INITIALIZE, {**request, "phase": "commit"})
        if response.get("phase") != "commit":
            raise SocketBridgeError("Initialize response did not announce commit phase")
        if not header.flags & protocol.MORE_FRAMES:
            raise SocketBridgeError("Initialize response did not announce InitialState")
        if protocol.sha256(response["effective_worker_config"]) != response["effective_worker_config_hash"]:
            raise SocketBridgeError("effective worker config hash mismatch")
        self.initialize_response = response
        self.layouts = {
            item["layout_kind"]: protocol.Layout.parse(item, self.num_slots) for item in response["layouts"]
        }
        initial_header, initial_payload = self._recv_frame()
        initial_layout = self.layouts["initial_state"]
        self._validate_response(initial_header, protocol.INITIAL_STATE, header.sequence, initial_layout.layout_id)
        state, faults, episodes = protocol.decode_state(initial_layout, self.state_schema, initial_payload)
        if faults.any():
            raise SocketBridgeError("InitialState contains Slot faults")
        self.initial_episode_indices = episodes
        if acknowledge_ready:
            manifest_hash = protocol.sha256(response)
            ready, _ = self._json_request(protocol.READY, {"manifest_hash": manifest_hash})
            if ready != {"ready": True}:
                raise SocketBridgeError("Ready acknowledgement failed")
        return state

    def write_action(self, actions: np.ndarray) -> None:
        """Store one finite Action batch with the negotiated shape.

        Args:
            actions: Finite float32 array with exactly ``(num_slots, 1)`` shape
                for the Cart-Pole test schema.

        Raises:
            ValueError: If shape or finiteness does not match.
        """

        value = np.asarray(actions, dtype=np.float32)
        if value.shape != self._actions.shape or not np.isfinite(value).all():
            raise ValueError(f"actions must be finite with shape {self._actions.shape}")
        self._actions[:] = value

    def step(self, step_decimation: int | None = None) -> tuple[np.ndarray, np.ndarray]:
        """Send the stored Action batch and return State plus Slot faults.

        Returns:
            Row-major StepResult values and one uint16 fault code per Slot.
        """

        request_layout = self.layouts["step_action"]
        result_layout = self.layouts["step_result"]
        payload = protocol.encode_action(
            request_layout,
            self.action_schema,
            self._actions,
            step_decimation=self.step_decimation if step_decimation is None else step_decimation,
        )
        header, result = self._binary_request(protocol.STEP, request_layout.layout_id, payload)
        self._validate_response(header, protocol.STEP, header.sequence, result_layout.layout_id)
        state, faults, _episodes = protocol.decode_state(result_layout, self.state_schema, result)
        return state, faults

    def reset(self, slots: list[int]) -> np.ndarray:
        """Reset selected Slots and return their post-reset State rows.

        Args:
            slots: Slot IDs to reset; IDs must be in the negotiated range.

        Returns:
            A copied row-major state array containing only the selected rows.
        """

        request_layout = self.layouts["reset_request"]
        result_layout = self.layouts["reset_result"]
        payload = protocol.encode_reset_mask(request_layout, self.num_slots, slots)
        header, result = self._binary_request(protocol.RESET, request_layout.layout_id, payload)
        self._validate_response(header, protocol.RESET, header.sequence, result_layout.layout_id)
        state, _faults, _episodes = protocol.decode_state(result_layout, self.state_schema, result)
        return state[slots].copy()

    def shutdown(self) -> dict[str, Any]:
        """Send the test shutdown request and close the socket.

        Returns:
            The Worker shutdown acknowledgement, or an empty mapping if no
            socket is currently open.

        Side effects:
            Close and clear the socket even when the request fails.
        """

        if self.sock is None:
            return {}
        try:
            response, _ = self._json_request(protocol.SHUTDOWN, {"reason": "test_complete"})
            return response
        finally:
            self.sock.close()
            self.sock = None

    def request_error(
        self,
        message: int,
        payload: dict[str, Any] | bytes,
        *,
        layout_id: int = 0,
        sequence: int | None = None,
        session: UUID | None = None,
        version: tuple[int, int] = protocol.PROTOCOL,
    ) -> dict[str, Any]:
        """Send one intentionally invalid request and return its stable Error DTO.

        Args:
            message: Protocol message kind to send.
            payload: JSON object or raw bytes intentionally violating a contract.
            layout_id: Layout identity encoded in the invalid header.
            sequence: Optional explicit sequence; defaults to the next sequence.
            session: Optional explicit session UUID; defaults to this client.
            version: Protocol version encoded in the header.

        Returns:
            The decoded Error DTO returned by the Worker.

        Raises:
            ConnectionError: If the client is not connected.
            SocketBridgeError: If the Worker does not return an Error frame.
        """
        if self.sock is None:
            raise ConnectionError("SocketBridge client is not connected")
        body = protocol.canonical_json(payload) if isinstance(payload, dict) else payload
        request = protocol.FrameHeader(
            version[0],
            version[1],
            message,
            protocol.REQUEST,
            len(body),
            self.sequence + 1 if sequence is None else sequence,
            self.session if session is None else session,
            layout_id,
        )
        self.sock.sendall(request.pack() + body)
        header, response = self._recv_frame()
        if header.message != protocol.ERROR or header.flags != protocol.RESPONSE | protocol.ERROR_FLAG:
            raise SocketBridgeError("invalid request did not receive an Error frame")
        return protocol.strict_loads(response)

    def request_header_error(self, message: int, *, payload_length: int, layout_id: int = 0) -> dict[str, Any]:
        """Send only an invalid Header and require rejection before Payload.

        Args:
            message: Protocol message kind in the malformed header.
            payload_length: Declared length intentionally used to test header
                validation without sending a body.
            layout_id: Layout identity encoded in the malformed header.

        Returns:
            The decoded stable Error DTO.
        """
        if self.sock is None:
            raise ConnectionError("SocketBridge client is not connected")
        request = protocol.FrameHeader(
            *protocol.PROTOCOL,
            message,
            protocol.REQUEST,
            payload_length,
            self.sequence + 1,
            self.session,
            layout_id,
        )
        self.sock.sendall(request.pack())
        header, response = self._recv_frame()
        if header.message != protocol.ERROR or header.flags != protocol.RESPONSE | protocol.ERROR_FLAG:
            raise SocketBridgeError("invalid Header did not receive an Error frame")
        return protocol.strict_loads(response)

    def _json_request(
        self, message: int, value: dict[str, Any], bootstrap: bool = False
    ) -> tuple[dict[str, Any], protocol.FrameHeader]:
        payload = protocol.canonical_json(value)
        header, response = self._request(message, 0, payload, bootstrap)
        return protocol.strict_loads(response), header

    def _binary_request(self, message: int, layout_id: int, payload: bytes) -> tuple[protocol.FrameHeader, bytes]:
        return self._request(message, layout_id, payload, False)

    def _request(
        self, message: int, layout_id: int, payload: bytes, bootstrap: bool
    ) -> tuple[protocol.FrameHeader, bytes]:
        if self.sock is None:
            raise ConnectionError("SocketBridge client is not connected")
        self.sequence += 1
        major, minor = (0, 0) if bootstrap else protocol.PROTOCOL
        request = protocol.FrameHeader(
            major, minor, message, protocol.REQUEST, len(payload), self.sequence, self.session, layout_id
        )
        self.sock.sendall(request.pack() + payload)
        header, response = self._recv_frame()
        if header.message == protocol.ERROR:
            error = protocol.strict_loads(response)
            raise SocketBridgeError(f"{error['code']} phase={error['phase']}: {error['message']}")
        self._validate_response(header, message, self.sequence, 0 if message < 0x100 else header.layout_id)
        return header, response

    def _recv_frame(self) -> tuple[protocol.FrameHeader, bytes]:
        assert self.sock is not None
        header = protocol.FrameHeader.unpack(self._recv_exact(protocol.HEADER_SIZE))
        return header, self._recv_exact(header.payload_length)

    def _recv_exact(self, length: int) -> bytes:
        assert self.sock is not None
        chunks: list[bytes] = []
        remaining = length
        while remaining:
            chunk = self.sock.recv(remaining)
            if not chunk:
                raise ConnectionError("SocketBridge disconnected during frame")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _validate_response(self, header: protocol.FrameHeader, message: int, sequence: int, layout_id: int) -> None:
        if header.flags & protocol.RESPONSE == 0 or header.message != message or header.sequence != sequence:
            raise SocketBridgeError("response envelope mismatch")
        if header.session != self.session or (header.major, header.minor) != protocol.PROTOCOL:
            raise SocketBridgeError("response protocol/session mismatch")
        if header.layout_id != layout_id:
            raise SocketBridgeError("response layout mismatch")

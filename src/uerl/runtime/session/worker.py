"""Orchestrate Config, Worker ownership, Manifest, and Bridge calls."""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, Protocol, cast

from ...core.codec import sha256_json
from ...core.codec.session import IBridgeSession, InitializeDescription, InitializeResult, SocketBridgeSession
from ...core.config.canonical import to_jsonable
from ...core.config.manifest import GitIdentityError, RunManifest, RunRecorder, capture_git_identity
from ...core.config.models import ResolvedRunConfig, SessionConfig
from ...core.config.robot import RobotSpec, merge_robot_spec
from ...errors import ConfigError, ProtocolError, SessionError
from .process import WorkerProcessController
from .state import SessionState


class BridgeFactory(Protocol):
    """Create one Bridge session for resolved connection settings."""

    def __call__(self, config: SessionConfig) -> IBridgeSession:
        """Create a synchronous Bridge client.

        Args:
            config: Resolved endpoint, timeout, and protocol settings.

        Returns:
            A disconnected Bridge client ready for ``connect``.
        """

        ...


@dataclass(frozen=True, slots=True)
class SessionInitialization:
    """Hold the Initialize result, merged RobotSpec, and Manifest identity.

    Attributes:
        bridge_result: Raw Initialize response and InitialState frame.
        robot_spec: Robot semantics bound to the UE asset topology.
        manifest_hash: Canonical identity written before Ready acknowledgement.
    """

    bridge_result: InitializeResult
    robot_spec: RobotSpec | None
    manifest_hash: str


class UERLSession:
    """Own one Worker session without implementing task mathematics.

    The Session is the orchestration boundary: it owns Worker launch/attach
    cleanup, persists Config and Manifest before Ready, and forwards raw typed
    protocol payloads. Task math and tensor conversion remain outside this class.
    """

    def __init__(
        self,
        config: ResolvedRunConfig,
        controller: WorkerProcessController,
        bridge: IBridgeSession,
        recorder: RunRecorder,
    ) -> None:
        self.config = config
        self._controller = controller
        self._bridge = bridge
        self._recorder = recorder
        self._state = SessionState.NEW
        self._initialization: SessionInitialization | None = None
        self._last_shutdown_response: dict[str, Any] | None = None
        self._description: tuple[InitializeDescription, RobotSpec | None] | None = None

    @classmethod
    def open(
        cls,
        config: ResolvedRunConfig,
        *,
        process_controller: WorkerProcessController | None = None,
        bridge_factory: BridgeFactory | None = None,
        recorder: RunRecorder | None = None,
    ) -> UERLSession:
        """Launch or attach a Worker and complete the Bridge Hello exchange.

        Args:
            config: Immutable resolved Run configuration defining ownership,
                endpoint, Worker executable, and protocol deadlines.
            process_controller: Optional ownership controller, primarily for
                dependency injection and deterministic lifecycle validation.
            bridge_factory: Optional factory for a synchronous Bridge client.
            recorder: Optional recorder for resolved Config and Manifest files.

        Returns:
            An OPEN Session after process ownership/attachment and Hello have
            completed. Initialize and Ready acknowledgement are still required.

        Raises:
            SessionError: If launch, attach, Bridge construction, or Hello
                connection fails.
            OSError: If an injected Bridge or process boundary propagates an
                operating-system failure.

        Side effects:
            Launches or records an attached Worker and opens a Bridge. Any
            failure closes resources already acquired and retains cleanup failures
            as notes on the original exception; no process-name scan or
            retry is performed.
        """

        controller = process_controller or WorkerProcessController()
        bridge_builder = bridge_factory or _default_bridge_factory
        run_recorder = recorder or RunRecorder(config.logging.run_directory)
        if config.session.mode.value == "launch":
            config.logging.run_directory.mkdir(parents=True, exist_ok=True)
            performance_path = (config.logging.run_directory.resolve() / "worker_stage_latency.jsonl").as_posix()
            controller.launch(
                config.session,
                extra_args=(f"-uerlperformancepath={performance_path}",),
            )
        else:
            controller.attach(config.session)
        try:
            bridge = bridge_builder(config.session)
        except BaseException as exc:
            _close_resources(controller, None, "bridge_connect_failed", exc)
            raise
        try:
            bridge.connect()
        except BaseException as exc:
            _close_resources(controller, bridge, "bridge_connect_failed", exc)
            raise
        session = cls(config, controller, bridge, run_recorder)
        session._state = SessionState.OPEN
        return session

    @property
    def state(self) -> SessionState:
        """Return the current Session lifecycle state.

        Returns:
            The externally visible state. FAILED remains distinguishable from
            CLOSED so callers can preserve the original failure reason.
        """

        return self._state

    @property
    def robot_spec(self) -> RobotSpec | None:
        """Return the topology-bound RobotSpec after a successful Initialize."""
        return self._initialization.robot_spec if self._initialization is not None else None

    @property
    def manifest_hash(self) -> str | None:
        """Return the persisted Manifest hash after initialize.

        Returns:
            The audit hash acknowledged by Ready, or ``None`` before a
            successful Initialize has persisted a Manifest.
        """

        return self._initialization.manifest_hash if self._initialization is not None else None

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest successful Step."""

        return self._bridge.last_step_timing

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest successful Reset."""

        return self._bridge.last_reset_timing

    @property
    def initialization(self) -> SessionInitialization | None:
        """Return the InitialState and Manifest record after initialize.

        Returns:
            The immutable SessionInitialization record after persistence, or
            ``None`` while the Session is only OPEN or has not initialized.
        """

        return self._initialization

    @property
    def last_shutdown_response(self) -> Mapping[str, Any] | None:
        """Return the latest Worker shutdown acknowledgement, if one exists."""

        return self._last_shutdown_response

    @property
    def described_robot_spec(self) -> RobotSpec | None:
        """Return the topology-bound RobotSpec after the describe phase."""

        return self._description[1] if self._description is not None else None

    @property
    def descriptor(self) -> Mapping[str, Any]:
        """Return the last UE Initialize description DTO."""

        if self._description is None:
            raise SessionError("Worker description is unavailable before Describe")
        return self._description[0].response

    def describe(self, schema_request: Mapping[str, Any]) -> InitializeDescription:
        """Reflect the Robot topology before committing Task field layouts."""

        if self._state is not SessionState.OPEN:
            error = SessionError(f"Describe is not valid in state {self._state.value}")
            self._fail("describe_in_invalid_state", error)
            raise error
        request = _build_initialize_request(self.config, schema_request)
        try:
            result = self._bridge.describe(request)
            robot_spec = _merge_robot_spec_if_configured(self.config, result)
        except (OSError, ProtocolError, SessionError) as exc:
            self._fail("describe_failed", exc)
            raise
        except ConfigError as exc:
            self._fail("robot_topology_merge_failed", exc)
            raise
        except ValueError as exc:
            self._fail("robot_topology_response_invalid", exc)
            raise ProtocolError("Invalid robot topology response") from exc
        self._description = (result, robot_spec)
        return result

    def initialize(self, schema_request: Mapping[str, Any]) -> InitializeResult:
        """Initialize the Worker and persist its Manifest before ReadyAck.

        Args:
            schema_request: The exact typed ``state_requirements`` and
                ``action_schema`` mapping supplied by the Direct Task seam.

        Returns:
            The raw Initialize response and InitialState frame. The response is
            retained in the SessionInitialization record.

        Raises:
            SessionError: If the Session state or schema request is invalid, or
                Manifest persistence cannot complete.
            ProtocolError: If the Worker response violates the protocol.

        Side effects:
            Send Initialize, build the sole Worker projection, atomically write
            resolved Config and Manifest, cache its hash, and move to
            INITIALIZED. Failure closes Bridge and Worker ownership.
        """

        if self._state is not SessionState.OPEN:
            error = SessionError(f"Initialize is not valid in state {self._state.value}")
            self._fail("initialize_in_invalid_state", error)
            raise error
        pending_description = self._description
        if pending_description is None:
            error = SessionError("Initialize commit requires a preceding Describe")
            self._fail("initialize_without_description", error)
            raise error
        request = _build_initialize_request(
            self.config,
            schema_request,
            robot_spec=pending_description[1],
        )
        try:
            result = self._bridge.initialize(request)
            _validate_effective_worker_config(self.config, result.response)
            robot_spec = pending_description[1]
            manifest = _build_manifest(self.config, result)
            self._recorder.write_resolved_config(self.config)
            manifest_hash = self._recorder.write_manifest_atomic(manifest)
        except (OSError, ProtocolError, SessionError, GitIdentityError) as exc:
            self._fail("initialize_failed", exc)
            raise
        except ConfigError as exc:
            self._fail("robot_topology_merge_failed", exc)
            raise
        except ValueError as exc:
            self._fail("robot_topology_response_invalid", exc)
            raise ProtocolError("Invalid robot topology response") from exc
        self._initialization = SessionInitialization(result, robot_spec, manifest_hash)
        self._description = None
        self._state = SessionState.INITIALIZED
        return result

    def acknowledge_ready(self) -> dict[str, Any]:
        """Send ReadyAck only after the Manifest has been persisted.

        Returns:
            The Worker Ready acknowledgement DTO.

        Raises:
            SessionError: If Initialize has not persisted a Manifest or the
                Session is not INITIALIZED.
            ProtocolError: If the Worker rejects the Manifest identity.

        Side effects:
            Send the persisted Manifest hash and move the Session to READY.
            Failure closes the Bridge and Worker ownership.
        """

        if self._state is not SessionState.INITIALIZED:
            error = SessionError(f"Ready is not valid in state {self._state.value}")
            self._fail("ready_in_invalid_state", error)
            raise error
        initialization = cast(SessionInitialization, self._initialization)
        try:
            response = self._bridge.acknowledge_ready(initialization.manifest_hash)
        except (OSError, ProtocolError, SessionError) as exc:
            self._fail("ready_failed", exc)
            raise
        self._state = SessionState.READY
        return response

    def step(self, layout_id: int, payload: bytes) -> tuple[Any, bytes]:
        """Forward one raw Step payload while Ready.

        Args:
            layout_id: Negotiated Step-action layout identity for ``payload``.
            payload: Boundary-validated binary Step action batch.

        Returns:
            The raw response header and Step-result bytes from the Worker.

        Raises:
            SessionError: If the Session is not READY or the exchange fails.
            ProtocolError: If the response envelope or payload violates the
                negotiated protocol.

        Side effects:
            Perform exactly one Worker Step. Any ambiguous transport or protocol
            failure transitions the Session to FAILED and closes ownership.
        """

        self._ensure_state(SessionState.READY)
        try:
            return self._bridge.step(layout_id, payload)
        except (OSError, ProtocolError, SessionError) as exc:
            self._fail("step_failed", exc)
            raise

    def reset(self, layout_id: int, payload: bytes) -> tuple[Any, bytes]:
        """Forward one raw Reset payload while Ready.

        Args:
            layout_id: Negotiated Reset-request layout identity for ``payload``.
            payload: Boundary-validated reset bitset batch.

        Returns:
            The raw response header and Reset-result bytes from the Worker.

        Raises:
            SessionError: If the Session is not READY or the exchange fails.
            ProtocolError: If the response envelope or payload violates the
                negotiated protocol.

        Side effects:
            Perform exactly one Worker Reset. Any ambiguous transport or
            protocol failure transitions the Session to FAILED and closes
            ownership without retrying the request.
        """

        self._ensure_state(SessionState.READY)
        try:
            return self._bridge.reset(layout_id, payload)
        except (OSError, ProtocolError, SessionError) as exc:
            self._fail("reset_failed", exc)
            raise

    def event(self, request: Mapping[str, Any]) -> Mapping[str, Any]:
        """Forward one typed event request to the Ready Worker."""

        self._ensure_state(SessionState.READY)
        try:
            return self._bridge.event(dict(request))
        except (OSError, ProtocolError, SessionError) as exc:
            self._fail("event_failed", exc)
            raise

    def close(self, reason: str = "session_close") -> None:
        """Shutdown the Bridge and close Worker resources according to ownership.

        Args:
            reason: Audit text sent to the Bridge and process cleanup path.

        Side effects:
            For an OPEN, INITIALIZED, or READY Session, request Bridge shutdown,
            close the transport, and terminate only a process owned by Python.
            Attached Workers remain running. Repeated calls are idempotent; a
            FAILED Session remains visibly FAILED after cleanup.
        """

        if self._state is SessionState.CLOSED:
            return
        error: BaseException | None = None
        try:
            if self._state in {SessionState.OPEN, SessionState.INITIALIZED, SessionState.READY}:
                self._last_shutdown_response = self._bridge.shutdown(reason)
        except BaseException as exc:
            error = exc
        finally:
            error = _close_resources(self._controller, self._bridge, reason, error)
            if self._state is not SessionState.FAILED:
                self._state = SessionState.CLOSED
        if error is not None:
            raise error

    def _fail(self, reason: str, error: BaseException) -> None:
        self._state = SessionState.FAILED
        _close_resources(self._controller, self._bridge, reason, error)

    def _ensure_state(self, *allowed: SessionState) -> None:
        if self._state not in allowed:
            raise SessionError(f"Session operation is not valid in state {self._state.value}")


def _close_resources(
    controller: WorkerProcessController,
    bridge: IBridgeSession | None,
    reason: str,
    error: BaseException | None = None,
) -> BaseException | None:
    """Attempt both Session cleanups and retain the first failure with later notes."""

    bridge_cleanup = (("Bridge", bridge.close),) if bridge is not None else ()
    for resource, close in (*bridge_cleanup, ("Worker", lambda: controller.close(reason))):
        try:
            close()
        except BaseException as cleanup_error:
            if error is None:
                error = cleanup_error
            else:
                error.add_note(f"{resource} cleanup failed: {cleanup_error!r}")
    return error


def _merge_robot_spec_if_configured(
    config: ResolvedRunConfig,
    result: InitializeResult | InitializeDescription,
) -> RobotSpec | None:
    """Bind configured Robot semantics to the topology returned by UE."""
    semantics = config.worker.robot_semantics
    if semantics is None:
        return None
    return merge_robot_spec(semantics, result.topology)


def _validate_effective_worker_config(
    config: ResolvedRunConfig,
    response: Mapping[str, Any],
) -> None:
    """Require the Worker to report the execution settings requested by Python."""

    effective = response.get("effective_worker_config")
    if not isinstance(effective, Mapping):
        raise ProtocolError("Worker effective configuration is missing")
    if effective.get("num_slots") != config.worker.slot_count:
        raise ProtocolError("Worker effective slot_count does not match the requested configuration")
    physics_dt = effective.get("physics_dt")
    if (
        not isinstance(physics_dt, (int, float))
        or isinstance(physics_dt, bool)
        or not math.isclose(float(physics_dt), config.worker.physics_dt, rel_tol=0.0, abs_tol=1.0e-12)
    ):
        raise ProtocolError("Worker effective physics_dt does not match the requested configuration")
    effective_decimation = effective.get("decimation")
    if (
        not isinstance(effective_decimation, list)
        or len(effective_decimation) != 2
        or any(type(value) is not int for value in effective_decimation)
        or effective_decimation[0] > effective_decimation[1]
        or tuple(effective_decimation) != config.worker.decimation
    ):
        raise ProtocolError("Worker effective decimation does not match the requested configuration")
    if effective.get("world_map") != config.session.map_path:
        raise ProtocolError("Worker effective world_map does not match the requested configuration")


def _default_bridge_factory(config: SessionConfig) -> IBridgeSession:
    return SocketBridgeSession(
        config.host,
        config.port,
        connect_timeout_s=config.connect_timeout_s,
        request_timeout_s=config.request_timeout_s,
        protocol_range=(config.protocol.major, config.protocol.min_minor, config.protocol.max_minor),
    )


def _build_initialize_request(
    config: ResolvedRunConfig,
    schema_request: Mapping[str, Any],
    *,
    robot_spec: RobotSpec | None = None,
) -> dict[str, Any]:
    """Build the sole Worker projection from resolved Config and Task schemas.

    The returned mapping is the only Config projection allowed to cross the
    Python-to-UE boundary; its hash covers the exact nested Worker values and
    schema lists sent in the Initialize request.
    """

    if set(schema_request) != {"state_requirements", "action_schema"}:
        raise SessionError("Initialize schema request must contain state_requirements and action_schema")
    if (
        not isinstance(schema_request["state_requirements"], list)
        or not isinstance(schema_request["action_schema"], list)
    ):
        raise SessionError("Initialize schema requirements must be arrays")
    worker = config.worker
    robot_config: dict[str, Any] = {
        "id": worker.robot_id,
        **({"asset_path": worker.robot_asset_path} if worker.robot_asset_path else {}),
        "scalars": worker.robot_config,
    }
    if robot_spec is not None:
        robot_config["actuators"] = [
            {
                "index": actuator.index,
                "joint_index": actuator.joint_index,
                "joint": actuator.joint,
                "coordinate": actuator.coordinate,
                "coordinate_type": actuator.coordinate_type,
                "unit": actuator.target_unit,
                "target_mode": actuator.target_mode,
                "stiffness": actuator.stiffness,
                "damping": actuator.damping,
                "effort_limit": actuator.effort_limit,
                "default_pos": actuator.default_pos,
            }
            for actuator in robot_spec.actuators
        ]
        robot_config["reset_bindings"] = [
            {
                "index": target.index,
                "name": target.wire_key,
                "target_type": target.target_type.value,
                "joint_index": target.joint_index if target.joint_index is not None else -1,
                "body_index": target.body_index if target.body_index is not None else -1,
                "component_index": target.component_index,
                "coordinate": (
                    robot_spec.topology.joints[target.joint_index].coordinate
                    if target.joint_index is not None
                    else "root"
                ),
                "unit": target.unit,
            }
            for target in robot_spec.reset
        ]
    worker_config = {
        "num_slots": worker.slot_count,
        "physics_dt": worker.physics_dt,
        "decimation": list(worker.decimation),
        "run_seed": str(worker.run_seed),
        "world_map": config.session.map_path,
        "environment": {
            "id": worker.environment_id,
            "scalars": worker.environment_config,
            **({"terrain": to_jsonable(worker.terrain_config)} if worker.terrain_config else {}),
        },
        "robot": robot_config,
    }
    return {
        "task_id": config.task_id,
        "task_version": config.task_version,
        "worker_config": worker_config,
        "worker_config_hash": sha256_json(worker_config),
        "state_requirements": schema_request["state_requirements"],
        "action_schema": schema_request["action_schema"],
    }


def _build_manifest(config: ResolvedRunConfig, result: InitializeResult) -> RunManifest:
    """Build the audit Manifest from resolved Config and Initialize response."""

    response = result.response
    selected_schemas = response["selected_schemas"]
    schema_hashes = {
        "state": selected_schemas["state_schema_hash"],
        "action": selected_schemas["action_schema_hash"],
    }
    layout_hashes = {item["layout_kind"]: item["layout_hash"] for item in response["layouts"]}
    return RunManifest(
        resolved_config=config,
        run_seed=config.worker.run_seed,
        run_directory=config.logging.run_directory,
        effective_worker_config=response["effective_worker_config"],
        build_identity=response["build_identity"],
        selected_protocol=response["selected_protocol"],
        schema_hashes=schema_hashes,
        layout_hashes=layout_hashes,
        seed_derivation_version=response["seed_derivation_version"],
        git_identity=capture_git_identity(),
    )

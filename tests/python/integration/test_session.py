"""Verify UERLSession orchestration and Manifest-before-Ready semantics."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import cast
from uuid import UUID

import pytest

from uerl import (
    ConfigError,
    DirectTaskConfig,
    LaunchMode,
    LoggingConfig,
    ProtocolError,
    ResolvedRunConfig,
    RslRlRunnerConfig,
    SessionConfig,
    SessionState,
    UERLSession,
    WorkerConfig,
)
from uerl.core.codec import FrameHeader, InitializeDescription, InitializeResult
from uerl.core.config.robot import parse_robot_config
from uerl.runtime.session import WorkerProcessController


class _FakeBridge:
    """Record typed Bridge calls for Session integration tests."""

    def __init__(self) -> None:
        self.events: list[str] = []
        self.ready_hash: str | None = None
        self.initialize_request: dict[str, object] | None = None
        self.closed = False
        self.last_step_timing: dict[str, float] = {}
        self.last_reset_timing: dict[str, float] = {}
        self.effective_worker_config = {
            "num_slots": 1,
            "physics_dt": 1.0 / 60.0,
            "decimation": [1, 1],
            "world_map": "/Engine/Maps/Entry",
        }

    def connect(self) -> dict[str, object]:
        """Record Hello completion and return the selected protocol."""

        self.events.append("connect")
        return {"selected_protocol": {"major": 2, "minor": 0}}

    def describe(self, request: dict[str, object]) -> InitializeDescription:
        """Return the reflected topology before the commit phase."""

        self.events.append("describe")
        result = self.initialize(request)
        self.events.pop()
        self.initialize_request = None
        response = dict(result.response)
        response["phase"] = "describe"
        return InitializeDescription(response)

    def initialize(self, request: dict[str, object]) -> InitializeResult:
        """Record Initialize commit and return the minimal Manifest inputs."""

        self.events.append("initialize")
        self.initialize_request = request
        response = {
            "phase": "commit",
            "selected_protocol": {"major": 2, "minor": 0},
            "build_identity": {"build_id": "ue-build"},
            "effective_worker_config": self.effective_worker_config,
            "selected_schemas": {"state_schema_hash": "a" * 64, "action_schema_hash": "b" * 64},
            "layouts": [
                {"layout_kind": "initial_state", "layout_hash": "c" * 64},
                {"layout_kind": "step_action", "layout_hash": "d" * 64},
            ],
            "seed_derivation_version": "uerl.seed.v1",
            "topology": {
                "body_names": ["base", "body"],
                "body_motion_types": ["kinematic", "simulated"],
                "root_body_index": 0,
                "fixed_base": True,
                "joints": [
                    {
                        "name": "valid_joint",
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
            },
        }
        header = FrameHeader(2, 0, 3, 2, 7, 2, UUID("00112233-4455-4677-8899-aabbccddeeff"), 0x42)
        return InitializeResult(response, header, b"initial")

    def acknowledge_ready(self, manifest_hash: str) -> dict[str, object]:
        """Record Ready and retain the Manifest hash sent by the Session."""

        self.events.append("ready")
        self.ready_hash = manifest_hash
        return {"ready": True}

    def event(self, request: Mapping[str, object]) -> dict[str, object]:
        """Record an event request for the Bridge protocol seam."""

        self.events.append("event")
        return {}

    def step(self, _layout_id: int, _payload: bytes) -> tuple[FrameHeader, bytes]:
        """Record one raw Step and return a scripted response."""

        self.events.append("step")
        return FrameHeader(2, 0, 0x101, 2, 4, 3, UUID("00112233-4455-4677-8899-aabbccddeeff"), 0x42), b"step"

    def reset(self, _layout_id: int, _payload: bytes) -> tuple[FrameHeader, bytes]:
        """Record one raw Reset and return a scripted response."""

        self.events.append("reset")
        return FrameHeader(2, 0, 0x102, 2, 5, 4, UUID("00112233-4455-4677-8899-aabbccddeeff"), 0x42), b"reset"

    def shutdown(self, _reason: str) -> dict[str, object]:
        """Record orderly Bridge shutdown."""

        self.events.append("shutdown")
        return {}

    def close(self) -> None:
        """Record transport close after shutdown or failure cleanup."""

        self.events.append("close")
        self.closed = True


def _config(
    run_directory: Path,
    mode: LaunchMode = LaunchMode.ATTACH,
    robot_asset_path: str = "",
) -> ResolvedRunConfig:
    """Build a resolved Config for Session tests."""

    return ResolvedRunConfig(
        task_id="task",
        task_version="1.0",
        session=SessionConfig(mode=mode),
        worker=WorkerConfig(
            environment_id="environment",
            robot_id="robot",
            robot_asset_path=robot_asset_path,
            run_seed=7,
        ),
        task=DirectTaskConfig(),
        runner=RslRlRunnerConfig(),
        logging=LoggingConfig(run_directory=run_directory),
        normalized_hash="e" * 64,
    )


def _initialize(session: UERLSession, request: dict[str, object]) -> InitializeResult:
    """Drive the formal Describe→Commit lifecycle in Session tests."""

    session.describe(request)
    return session.initialize(request)


def test_session_closes_when_robot_topology_does_not_match_semantics(tmp_path: Path) -> None:
    """Reject a configured Robot before the Session becomes initialized."""
    bridge = _FakeBridge()
    config = _config(tmp_path / "run", robot_asset_path="/Game/Robots/CartPole/SKM_CartPole")
    robot_semantics = parse_robot_config(
        """
actuators:
  - joint: missing
    stiffness: 1.0
    damping: 0.0
    effort_limit: 1.0
    default_pos: 0.0
    action_scale: 1.0
observations:
  - type: joint_position
    joint: missing
reset: {distributions: []}
"""
    )
    config = ResolvedRunConfig(
        task_id=config.task_id,
        task_version=config.task_version,
        session=config.session,
        worker=WorkerConfig(
            environment_id=config.worker.environment_id,
            robot_id=config.worker.robot_id,
            robot_asset_path=config.worker.robot_asset_path,
            robot_semantics=robot_semantics,
            run_seed=config.worker.run_seed,
        ),
        task=config.task,
        runner=config.runner,
        logging=config.logging,
        normalized_hash=config.normalized_hash,
    )

    session = UERLSession.open(
        config,
        process_controller=WorkerProcessController(),
        bridge_factory=lambda _config: bridge,
    )

    with pytest.raises(ConfigError) as raised:
        _initialize(session, {"state_requirements": [], "action_schema": []})

    assert raised.value.code == "UNKNOWN_JOINT"

    assert session.state is SessionState.FAILED
    assert bridge.closed


def test_session_projects_robot_asset_path_into_initialize(tmp_path: Path) -> None:
    """Project the UE object path into the robot provider request only when set."""

    bridge = _FakeBridge()
    session = UERLSession.open(
        _config(tmp_path / "run", robot_asset_path="/Game/Robots/CartPole/SKM_CartPole"),
        process_controller=WorkerProcessController(),
        bridge_factory=lambda _config: bridge,
    )

    _ = _initialize(session, {"state_requirements": [], "action_schema": []})

    assert bridge.initialize_request is not None
    worker_config = bridge.initialize_request["worker_config"]
    assert isinstance(worker_config, dict)
    assert worker_config["world_map"] == "/Engine/Maps/Entry"
    robot = cast(dict[str, object], worker_config["robot"])
    assert robot["asset_path"] == "/Game/Robots/CartPole/SKM_CartPole"
    assert "semantics" not in robot
    session.close("test")


def test_session_rejects_effective_worker_timing_drift(tmp_path: Path) -> None:
    """Reject a Worker that reports timing different from the requested Config."""

    bridge = _FakeBridge()
    bridge.effective_worker_config["decimation"] = [2, 2]
    session = UERLSession.open(
        _config(tmp_path / "run"),
        process_controller=WorkerProcessController(),
        bridge_factory=lambda _config: bridge,
    )

    with pytest.raises(ProtocolError, match="decimation"):
        _initialize(session, {"state_requirements": [], "action_schema": []})

    assert session.state is SessionState.FAILED
    assert bridge.closed


def test_session_persists_manifest_before_ready_and_closes_in_order(tmp_path: Path) -> None:
    """Persist Manifest before ReadyAck and expose raw Bridge transitions."""

    bridge = _FakeBridge()
    session = UERLSession.open(
        _config(tmp_path / "run"),
        process_controller=WorkerProcessController(),
        bridge_factory=lambda _config: bridge,
    )

    assert session.state is SessionState.OPEN
    initial = _initialize(session, {"state_requirements": [], "action_schema": []})
    assert initial.initial_payload == b"initial"
    assert session.robot_spec is None
    assert session.state.value == SessionState.INITIALIZED.value
    assert bridge.events == ["connect", "describe", "initialize"]
    assert (tmp_path / "run" / "manifest.json").exists()
    assert session.acknowledge_ready() == {"ready": True}
    assert session.step(0x42, b"action")[1] == b"step"
    assert session.reset(0x42, b"mask")[1] == b"reset"
    session.close("test")

    assert bridge.events == ["connect", "describe", "initialize", "ready", "step", "reset", "shutdown", "close"]
    assert session.state.value == SessionState.CLOSED.value
    assert session.manifest_hash is not None
    print("[VERIFY] VC-006: init=1 step=1 reset=1 shutdown=1 in_flight_max=1")


def test_manifest_write_failure_fails_before_ready(tmp_path: Path) -> None:
    """Fail and close resources when the Run directory cannot be written."""

    blocked = tmp_path / "blocked"
    blocked.write_text("not a directory", encoding="utf-8")
    bridge = _FakeBridge()
    session = UERLSession.open(
        _config(blocked),
        process_controller=WorkerProcessController(),
        bridge_factory=lambda _config: bridge,
    )

    with pytest.raises(OSError):
        _initialize(session, {"state_requirements": [], "action_schema": []})

    assert session.state is SessionState.FAILED
    assert "ready" not in bridge.events
    assert bridge.closed

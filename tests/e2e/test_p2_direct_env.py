"""Verify the product CartPole DirectEnv lifecycle against real UE."""

from __future__ import annotations

import json
from collections.abc import Mapping
from dataclasses import replace
from pathlib import Path

import pytest
import torch

from tests.e2e.support.worker_runner import TRAIN_MAP, UE_CMD, UPROJECT
from uerl import (
    ResolvedRunConfig,
    SessionState,
    StepContext,
    TerminationResult,
    UERLDirectEnv,
    UERLSession,
    UERLSessionAdapter,
)
from uerl.core.config.robot import ObservationConfig, ObsType
from uerl.core.config.yaml_loader import load_unique_yaml
from uerl.core.direct.task import DirectTask
from uerl.core.direct.types import SessionSchema
from uerl.core.inputs import InputSpec, RayGroundFactory
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.cartpole import (
    CARTPOLE_CONTROL_DT,
    CARTPOLE_TASK_ID,
    CartPoleTaskConfig,
    build_cartpole_composed_cfg,
)
from uerl.training import build_launch_overrides, build_run_config

SLOT_COUNT = 64
RAY = RayGroundFactory().validate(InputSpec(
    "ground_probe", "uerl.ray_ground", 1, attachment="pole",
    parameters={"offsets_m": [[0.0, 0.0], [0.25, 0.0]], "alignment": "world"},
))


class _SparseResetCartPoleTask(DirectTask):
    """Keep product RobotSpec binding while making sparse Reset deterministic."""

    def __init__(self, config: object, declared_inputs: bool = False) -> None:
        self.declared_inputs = declared_inputs
        params = CartPoleTaskConfig.from_direct(config)  # type: ignore[arg-type]
        super().__init__(
            control_dt=CARTPOLE_CONTROL_DT,
            batch_size=SLOT_COUNT,
            device="cpu",
            cfg_factory=lambda spec: build_cartpole_composed_cfg(params, spec),
            task_config=params,
        )

    @property
    def schema(self) -> SessionSchema:
        base = super().schema
        if self.declared_inputs and base.state_requirements:
            return SessionSchema(
                base.state_requirements + tuple(f.to_mapping() for f in RAY.fields), base.action_schema,
            )
        return base

    def build_observations(
        self, raw_state: Mapping[str, torch.Tensor], state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor, control_frame_dt: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        observations = dict(super().build_observations(
            raw_state, state_valid, previous_policy_actions, control_frame_dt,
        ))
        if self.declared_inputs:
            observations["rays"] = raw_state["input.ground_probe.height"].clone()
            observations["pole_pose"] = raw_state["robot.body.pole.body_pose"].clone()
        return observations

    def compute_terminations(self, context: StepContext) -> TerminationResult:
        """Terminate row zero each step and time out the remaining rows at the limit."""

        terminated = torch.zeros(context.episode_steps.shape[0], dtype=torch.bool)
        terminated[0] = context.episode_steps[0] == 1
        return TerminationResult(
            terminated,
            context.episode_steps >= self.max_episode_steps,
        )

    def compute_rewards(self, context: StepContext, terminations: TerminationResult) -> torch.Tensor:
        """Return one neutral reward per valid row for lifecycle-only evidence."""

        return torch.ones(context.episode_steps.shape[0])


def _config(run_directory: Path, declared_inputs: bool = False) -> ResolvedRunConfig:
    """Resolve the canonical generic CartPole product with the acceptance batch."""

    overrides = build_launch_overrides(
        ue_executable=Path(UE_CMD),
        project=Path(UPROJECT),
        map_name=TRAIN_MAP,
    )
    overrides.update(
        {
            "worker.slot_count": str(SLOT_COUNT),
            "task.max_episode_steps": "2",
            "logging.run_directory": str(run_directory),
        }
    )
    if declared_inputs:
        overrides["worker.inputs"] = json.dumps([
            InputSpec("proprioception", "uerl.state", 1, parameters={"fields": [
                "robot.joint.pole.joint_position", "robot.joint.cart.joint_velocity",
            ]}).to_mapping(),
            RAY.spec.to_mapping(),
        ])
    config = build_run_config(CARTPOLE_TASK_ID, overrides=overrides)
    if declared_inputs:
        semantics = config.worker.robot_semantics
        assert semantics is not None
        config = replace(config, worker=replace(config.worker, robot_semantics=replace(
            semantics, observations=semantics.observations + (ObservationConfig(ObsType.BODY_POSE, "pole", "body"),),
        )))
    return config


@pytest.mark.parametrize("declared_inputs", [False, True], ids=["legacy", "declared-inputs"])
def test_ac_ue_generic_013_cartpole_completes_real_ue_lifecycle(tmp_path: Path, declared_inputs: bool) -> None:
    """Prove Describe, generic Commit, Step, sparse Reset, and shutdown against UE."""

    config = _config(tmp_path / "run", declared_inputs)
    raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
    task = _SparseResetCartPoleTask(config.task, declared_inputs)
    env = UERLDirectEnv(
        UERLSessionAdapter(raw_session),
        task,
        resolved_config=config,
    )
    try:
        manifest_value = load_unique_yaml((config.logging.run_directory / "manifest.yaml").read_text(encoding="utf-8"))
        assert isinstance(manifest_value, dict)
        manifest: dict[str, object] = manifest_value
        resolved_config = manifest.get("resolved_config")
        assert isinstance(resolved_config, dict)
        state_names = tuple(field["name"] for field in task.schema.state_requirements)
        action_names = tuple(field["name"] for field in task.schema.action_schema)
        assert raw_session.state is SessionState.READY
        assert config.worker.robot_id == "uerl.robot.skeletal_mesh"
        assert config.worker.environment_id == "uerl.environment.shared_world"
        assert task.robot_spec is not None
        assert state_names == (
            "robot.joint.pole.joint_position",
            "robot.joint.pole.joint_velocity",
            "robot.joint.cart.joint_position",
            "robot.joint.cart.joint_velocity",
        ) + (("robot.body.pole.body_pose", "input.ground_probe.height") if declared_inputs else ())
        if declared_inputs:
            assert len(raw_session.input_requirements) == 2
            assert raw_session.input_requirements[1] == RAY
        assert action_names == ("robot.actuator.target",)
        assert resolved_config["normalized_hash"] == config.normalized_hash
        assert manifest["run_seed"] == config.worker.run_seed
        assert manifest["layout_hashes"]
        initial_obs = {key: value.clone() for key, value in env.get_observations().items()}
        initial_policy = initial_obs["policy"]
        first_actions = torch.zeros(SLOT_COUNT, 1)
        first_actions[1, 0] = 0.5
        first_obs, first_reward, first_terminated, first_truncated, first_info = env.step(first_actions)
        first_episode_length_buf = env.episode_length_buf.clone()
        second_obs, second_reward, second_terminated, second_truncated, second_info = env.step(
            torch.zeros(SLOT_COUNT, 1)
        )
    finally:
        env.close("p2_generic_direct_env_e2e")

    expected_terminated = torch.zeros(SLOT_COUNT, dtype=torch.bool)
    expected_terminated[0] = True
    expected_truncated = torch.ones(SLOT_COUNT, dtype=torch.bool)
    expected_truncated[0] = False
    expected_first_lengths = torch.ones(SLOT_COUNT, dtype=torch.long)
    expected_first_lengths[0] = 0
    assert abs(float(initial_policy[1, 3])) < 1.0e-6
    assert abs(float(first_obs["policy"][1, 3])) > 1.0e-4
    assert first_obs["policy"].shape == (SLOT_COUNT, 4)
    assert torch.equal(first_reward, torch.ones(SLOT_COUNT))
    assert torch.equal(first_terminated, expected_terminated)
    assert not bool(first_truncated.any())
    assert torch.equal(first_episode_length_buf, expected_first_lengths)
    assert torch.equal(second_reward, torch.ones(SLOT_COUNT))
    assert torch.equal(second_terminated, expected_terminated)
    assert torch.equal(second_truncated, expected_truncated)
    assert torch.equal(env.episode_length_buf, torch.zeros(SLOT_COUNT, dtype=torch.long))
    assert second_obs["policy"].shape == (SLOT_COUNT, 4)
    assert second_info["terminal_observation"]["policy"].shape == (SLOT_COUNT, 4)
    if declared_inputs:
        # The procedural plane is at z=0. Body telemetry is an independent SI
        # oracle for each ray's signed height relative to the pole attachment.
        for observations in (initial_obs, first_obs, second_obs):
            _assert_ground_height(observations["rays"], observations["pole_pose"])
        for info in (first_info, second_info):
            raw = info["terminal_raw_state"]
            _assert_ground_height(raw["input.ground_probe.height"], raw["robot.body.pole.body_pose"])
        first_terminal = first_info["terminal_raw_state"]["input.ground_probe.height"]
        torch.testing.assert_close(first_obs["rays"][1:], first_terminal[1:], atol=0, rtol=0)
        assert not second_info["slot_fault_code"].any()
    assert raw_session.state is SessionState.CLOSED  # type: ignore[comparison-overlap]
    print("[VERIFY] AC-UE-GENERIC-013: slots=64 action=nonzero step=2 sparse_reset=1 shutdown=PASS")


def _assert_ground_height(rays: torch.Tensor, pose: torch.Tensor) -> None:
    assert rays.shape == (SLOT_COUNT, 2)
    assert torch.all(pose[:, 2].abs() > 0.05)
    torch.testing.assert_close(rays, -pose[:, 2:3].expand(-1, 2), atol=1e-5, rtol=0)

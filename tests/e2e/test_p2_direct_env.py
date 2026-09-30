"""Verify the product CartPole DirectEnv lifecycle against real UE."""

from __future__ import annotations

import json
from pathlib import Path

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
from uerl.core.direct.task import DirectTask
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.cartpole import (
    CARTPOLE_CONTROL_DT,
    CARTPOLE_TASK_ID,
    CartPoleTaskConfig,
    build_cartpole_composed_cfg,
)
from uerl.training import build_launch_overrides, build_run_config

SLOT_COUNT = 64


class _SparseResetCartPoleTask(DirectTask):
    """Keep product RobotSpec binding while making sparse Reset deterministic."""

    def __init__(self, config: object) -> None:
        params = CartPoleTaskConfig.from_direct(config)  # type: ignore[arg-type]
        super().__init__(
            control_dt=CARTPOLE_CONTROL_DT,
            batch_size=SLOT_COUNT,
            device="cpu",
            cfg_factory=lambda spec: build_cartpole_composed_cfg(params, spec),
            task_config=params,
        )

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


def _config(run_directory: Path) -> ResolvedRunConfig:
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
    return build_run_config(CARTPOLE_TASK_ID, overrides=overrides)


def test_ac_ue_generic_013_cartpole_completes_real_ue_lifecycle(tmp_path: Path) -> None:
    """Prove Describe, generic Commit, Step, sparse Reset, and shutdown against UE."""

    config = _config(tmp_path / "run")
    raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
    task = _SparseResetCartPoleTask(config.task)
    env = UERLDirectEnv(
        UERLSessionAdapter(raw_session),
        task,
        resolved_config=config,
    )
    try:
        manifest = json.loads((config.logging.run_directory / "manifest.json").read_text(encoding="utf-8"))
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
        )
        assert action_names == ("robot.actuator.target",)
        assert manifest["resolved_config"]["normalized_hash"] == config.normalized_hash
        assert manifest["run_seed"] == config.worker.run_seed
        assert manifest["layout_hashes"]
        initial_policy = env.get_observations()["policy"].clone()
        first_actions = torch.zeros(SLOT_COUNT, 1)
        first_actions[1, 0] = 0.5
        first_obs, first_reward, first_terminated, first_truncated, _first_info = env.step(
            first_actions
        )
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
    assert raw_session.state is SessionState.CLOSED  # type: ignore[comparison-overlap]
    print("[VERIFY] AC-UE-GENERIC-013: slots=64 action=nonzero step=2 sparse_reset=1 shutdown=PASS")

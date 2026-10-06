"""Verify external minimal and Manager Tasks share DirectEnv behavior."""

from __future__ import annotations

import importlib
import sys
from collections.abc import Callable, Mapping
from dataclasses import dataclass, replace
from importlib.metadata import EntryPoint, EntryPoints
from pathlib import Path
from typing import Any, TypeVar, cast

import pytest
import torch

from tests.python.unit.test_cartpole_product_task import CARTPOLE_SHAPES, _robot_spec
from uerl import (
    CartPoleTaskConfig,
    InitialState,
    PhysicalCommandBatch,
    PostResetState,
    SessionSchema,
    TransitionState,
    UERLDirectEnv,
    robot_actuator_action_schema,
    robot_observation_schema,
)
from uerl.core.config.models import DirectTaskConfig
from uerl.core.config.robot import RobotSpec
from uerl.core.direct.capabilities import CapabilityStatus
from uerl.core.direct.robot_observation import ObservationShapeTable
from uerl.core.direct.task import DirectTask
from uerl.core.direct.types import StateBatch
from uerl.core.mdp.lib.events import EventEffect
from uerl.tasks.cartpole import (
    create_cartpole_registration,
)
from uerl.tasks.registry import TaskRegistration, create_default_registry

EXTERNAL_TASK_ID = "Test-CartPole-Minimal-v0"
EXTERNAL_ENTRY_POINT = "test-minimal-cartpole"
EXTERNAL_MANAGER_TASK_ID = "Test-CartPole-Manager-v0"
EXTERNAL_MANAGER_ENTRY_POINT = "test-manager-cartpole"
_OBSERVATION_FIELDS = (
    "robot.joint.pole.joint_position",
    "robot.joint.pole.joint_velocity",
    "robot.joint.cart.joint_position",
    "robot.joint.cart.joint_velocity",
)
_ACTION_FIELD = "robot.actuator.target"
_SLOT_COUNT = 4
_StateBatchT = TypeVar("_StateBatchT", bound=StateBatch)


def _external_config() -> CartPoleTaskConfig:
    """Represent a user reward-only edit without depending on the generator."""

    return replace(CartPoleTaskConfig(), rew_scale_pole_pos=-2.0, max_episode_steps=3)


def _create_external_minimal_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
) -> DirectTask:
    package_source = Path(__file__).resolve().parents[3] / "examples" / "external-direct-cartpole" / "src"
    package_source_text = str(package_source)
    inserted = package_source_text not in sys.path
    if inserted:
        sys.path.insert(0, package_source_text)
    sys.modules.pop("example_direct_cartpole", None)
    try:
        direct_example = importlib.import_module("example_direct_cartpole")
        task = direct_example.create_task(CartPoleTaskConfig.from_direct(config), robot_spec=robot_spec)
        return cast(DirectTask, task)
    finally:
        sys.modules.pop("example_direct_cartpole", None)
        if inserted:
            sys.path.remove(package_source_text)


def create_external_minimal_registration() -> TaskRegistration:
    """Entry-point fixture used to exercise real external registry discovery."""

    return replace(
        create_cartpole_registration(),
        task_id=EXTERNAL_TASK_ID,
        task_version="0.1.0",
        task_factory=_create_external_minimal_task,
        task_config_factory=_external_config,
    )


def create_external_manager_registration() -> TaskRegistration:
    """Expose the composed Manager path through a second external entry point."""

    return replace(
        create_cartpole_registration(),
        task_id=EXTERNAL_MANAGER_TASK_ID,
        task_version="0.1.0",
        task_config_factory=_external_config,
    )


@dataclass(frozen=True)
class _StepResult:
    observations: dict[str, torch.Tensor]
    rewards: torch.Tensor
    terminated: torch.Tensor
    truncated: torch.Tensor
    info: dict[str, Any]


@dataclass(frozen=True)
class _Trajectory:
    initial_observations: dict[str, torch.Tensor]
    steps: tuple[_StepResult, ...]
    session: _ScriptedCartPoleSession


class _ScriptedCartPoleSession:
    """Return reviewed state cases and retain action/reset effects at the seam."""

    def __init__(self) -> None:
        self.num_slots = _SLOT_COUNT
        self.robot_spec = _robot_spec()
        self.observation_shapes = CARTPOLE_SHAPES
        self.descriptor = {
            "available_state_schema": list(
                robot_observation_schema(self.robot_spec, self.observation_shapes)
            ),
            "available_action_schema": list(robot_actuator_action_schema(self.robot_spec)),
        }
        self.initial_state = _make_batch(
            InitialState,
            ((0.0, 0.0, 0.0, 0.0),) * _SLOT_COUNT,
            episode_index=(0, 0, 0, 0),
        )
        self.reset_states: tuple[PostResetState, ...] = (
            _make_batch(
                PostResetState,
                ((0.0, 0.0, 0.0, 0.0),) * _SLOT_COUNT,
                episode_index=(0, 0, 0, 0),
            ),
            _make_batch(
                PostResetState,
                (
                    (20.0, 21.0, 22.0, 23.0),
                    (-0.2, 0.0, 1.25, 0.0),
                    (30.0, 31.0, 32.0, 33.0),
                    (0.0, 0.0, 0.0, 0.0),
                ),
                episode_index=(0, 1, 0, 1),
            ),
            _make_batch(
                PostResetState,
                (
                    (0.0, 0.0, -0.1, 0.0),
                    (40.0, 41.0, 42.0, 43.0),
                    (0.0, 0.0, 0.2, 0.0),
                    (50.0, 51.0, 52.0, 53.0),
                ),
                episode_index=(1, 1, 1, 1),
            ),
        )
        self.transitions: tuple[TransitionState, ...] = (
            _make_batch(
                TransitionState,
                (
                    (0.1, 0.2, 0.2, 0.4),
                    (0.4, 0.3, 3.1, 0.5),
                    (0.2, -0.5, -0.5, 0.6),
                    (100.0, 100.0, 100.0, 100.0),
                ),
                valid=(True, True, True, False),
                fault_codes=(0, 0, 0, 9),
                episode_index=(0, 0, 0, 0),
            ),
            _make_batch(
                TransitionState,
                (
                    (0.2, 0.1, 0.3, 0.2),
                    (0.05, 0.2, -0.3, 0.1),
                    (-0.1, 0.4, 1.0, -0.2),
                    (0.3, -0.2, 0.2, 0.3),
                ),
                episode_index=(0, 1, 0, 1),
            ),
        )
        self.step_commands: list[torch.Tensor] = []
        self.reset_masks: list[torch.Tensor] = []
        self._transition_index = 0
        self.close_calls = 0

    def initialize_with_robot_spec(
        self,
        schema: SessionSchema,
        bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
    ) -> InitialState:
        del schema
        bind_robot_spec(self.robot_spec, self.observation_shapes)
        return self.initial_state

    def acknowledge_ready(self) -> None:
        pass

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        return {}

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        return {}

    def step(
        self,
        command: PhysicalCommandBatch,
        step_decimation: int | None = None,
    ) -> TransitionState:
        assert step_decimation == 1
        self.step_commands.append(command[_ACTION_FIELD].clone())
        state = self.transitions[self._transition_index]
        self._transition_index += 1
        return state

    def reset(
        self,
        reset_mask: torch.Tensor,
        terrain_levels: torch.Tensor | None = None,
        reset_values: torch.Tensor | None = None,
    ) -> PostResetState:
        del terrain_levels, reset_values
        self.reset_masks.append(reset_mask.clone())
        return self.reset_states[len(self.reset_masks) - 1]

    def close(self, reason: str = "session_close") -> None:
        del reason
        self.close_calls += 1

    def apply_event(self, effect: EventEffect) -> Mapping[str, Any]:
        return {"applied": True, "kind": effect.kind}


def _make_batch(
    batch_type: type[_StateBatchT],
    rows: tuple[tuple[float, float, float, float], ...],
    *,
    valid: tuple[bool, ...] = (True, True, True, True),
    fault_codes: tuple[int, ...] = (0, 0, 0, 0),
    episode_index: tuple[int, ...],
) -> _StateBatchT:
    matrix = torch.tensor(rows, dtype=torch.float32)
    values = {name: matrix[:, index : index + 1] for index, name in enumerate(_OBSERVATION_FIELDS)}
    return batch_type(
        values,
        torch.tensor(valid, dtype=torch.bool),
        torch.tensor(fault_codes, dtype=torch.uint16),
        torch.tensor(episode_index, dtype=torch.uint64),
    )


def _run_scripted_trajectory(task: DirectTask, *, scalar_fields: bool = False) -> _Trajectory:
    session = _ScriptedCartPoleSession()
    if scalar_fields:
        def flatten(batch: _StateBatchT) -> _StateBatchT:
            return replace(batch, values={name: value.reshape(-1) for name, value in batch.values.items()})

        session.initial_state = flatten(session.initial_state)
        session.reset_states = tuple(flatten(batch) for batch in session.reset_states)
        session.transitions = tuple(flatten(batch) for batch in session.transitions)
    env = UERLDirectEnv(session, task)
    try:
        initial = dict(env.get_observations())
        actions = (
            torch.tensor([[0.25], [2.0], [-0.5], [1.5]]),
            torch.tensor([[0.1], [-0.2], [0.3], [0.4]]),
        )
        results = []
        for action in actions:
            observations, rewards, terminated, truncated, info = env.step(action)
            results.append(
                _StepResult(dict(observations), rewards, terminated, truncated, info)
            )
        return _Trajectory(initial, tuple(results), session)
    finally:
        env.close()


def _assert_reviewed_behavior(result: _Trajectory, *, edited_reward: bool) -> None:
    pole_weight = -2.0 if edited_reward else -1.0
    first_rewards = torch.tensor(
        [
            1.0 + pole_weight * 0.01 - 0.004 - 0.001,
            -2.0 + pole_weight * 0.16 - 0.005 - 0.0015,
            1.0 + pole_weight * 0.04 - 0.006 - 0.0025,
            -2.0,
        ]
    )
    second_rewards = torch.tensor(
        [
            1.0 + pole_weight * 0.04 - 0.002 - 0.0005,
            1.0 + pole_weight * 0.0025 - 0.001 - 0.001,
            1.0 + pole_weight * 0.01 - 0.002 - 0.002,
            1.0 + pole_weight * 0.09 - 0.003 - 0.001,
        ]
    )
    first, second = result.steps
    torch.testing.assert_close(
        result.initial_observations["policy"], torch.zeros(_SLOT_COUNT, 4)
    )
    torch.testing.assert_close(first.rewards, first_rewards, atol=1.0e-6, rtol=0.0)
    assert torch.equal(first.terminated, torch.tensor([False, True, False, True]))
    assert torch.equal(first.truncated, torch.tensor([False, False, False, False]))
    torch.testing.assert_close(
        first.observations["policy"],
        torch.tensor(
            [
                [0.1, 0.2, 0.2, 0.4],
                [-0.2, 0.0, 1.25, 0.0],
                [0.2, -0.5, -0.5, 0.6],
                [0.0, 0.0, 0.0, 0.0],
            ]
        ),
        atol=1.0e-6,
        rtol=0.0,
    )
    torch.testing.assert_close(
        first.info["terminal_observation"]["policy"][1],
        torch.tensor([0.4, 0.3, 3.1, 0.5]),
    )
    torch.testing.assert_close(
        first.info["terminal_observation"]["policy"][3],
        torch.zeros(4),
    )
    assert torch.equal(first.info["terminal_observation_valid"], torch.tensor([True, True, True, False]))
    assert first.info["terminal_raw_state"][_OBSERVATION_FIELDS[0]][3].item() == 100.0
    assert torch.equal(first.info["episode_index"], torch.tensor([0, 1, 0, 1], dtype=torch.uint64))

    torch.testing.assert_close(second.rewards, second_rewards, atol=1.0e-6, rtol=0.0)
    assert torch.equal(second.terminated, torch.tensor([False, False, False, False]))
    assert torch.equal(second.truncated, torch.tensor([True, False, True, False]))
    torch.testing.assert_close(
        second.observations["policy"],
        torch.tensor(
            [
                [0.0, 0.0, -0.1, 0.0],
                [0.05, 0.2, -0.3, 0.1],
                [0.0, 0.0, 0.2, 0.0],
                [0.3, -0.2, 0.2, 0.3],
            ]
        ),
        atol=1.0e-6,
        rtol=0.0,
    )
    assert torch.equal(second.info["episode_index"], torch.ones(_SLOT_COUNT, dtype=torch.uint64))
    assert torch.equal(
        result.session.reset_masks[0], torch.ones(_SLOT_COUNT, dtype=torch.bool)
    )
    assert torch.equal(
        result.session.reset_masks[1], torch.tensor([False, True, False, True])
    )
    assert torch.equal(
        result.session.reset_masks[2], torch.tensor([True, False, True, False])
    )
    torch.testing.assert_close(
        result.session.step_commands[0], torch.tensor([[25.0], [100.0], [-50.0], [100.0]])
    )
    torch.testing.assert_close(
        result.session.step_commands[1], torch.tensor([[10.0], [-20.0], [30.0], [40.0]])
    )
    assert result.session.close_calls == 1


def _assert_same_trajectory(left: _Trajectory, right: _Trajectory) -> None:
    for first, second in zip(left.steps, right.steps, strict=True):
        torch.testing.assert_close(first.observations["policy"], second.observations["policy"])
        torch.testing.assert_close(first.rewards, second.rewards)
        assert torch.equal(first.terminated, second.terminated)
        assert torch.equal(first.truncated, second.truncated)
        torch.testing.assert_close(
            first.info["terminal_observation"]["policy"],
            second.info["terminal_observation"]["policy"],
        )


def test_external_minimal_and_manager_entrypoints_match_behavior_and_reward_edit(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    points = EntryPoints(
        [
            EntryPoint(
                name=EXTERNAL_ENTRY_POINT,
                value=(
                    "tests.python.integration.test_direct_task_entrypoint_equivalence:"
                    "create_external_minimal_registration"
                ),
                group="uerl.tasks",
            ),
            EntryPoint(
                name=EXTERNAL_MANAGER_ENTRY_POINT,
                value=(
                    "tests.python.integration.test_direct_task_entrypoint_equivalence:"
                    "create_external_manager_registration"
                ),
                group="uerl.tasks",
            ),
        ]
    )
    monkeypatch.setattr("importlib.metadata.entry_points", lambda **kwargs: points)
    registry = create_default_registry(
        external_tasks=(EXTERNAL_ENTRY_POINT, EXTERNAL_MANAGER_ENTRY_POINT)
    )

    external_config = registry.create_task_config(EXTERNAL_TASK_ID)
    manager_defaults = cast(
        CartPoleTaskConfig,
        registry.create_task_config(EXTERNAL_MANAGER_TASK_ID),
    )
    manager_config = replace(manager_defaults, rew_scale_pole_pos=-2.0, max_episode_steps=3)
    baseline_config = replace(manager_config, rew_scale_pole_pos=-1.0)
    external_task = registry.create_task(EXTERNAL_TASK_ID, external_config)
    manager_task = registry.create_task(EXTERNAL_MANAGER_TASK_ID, manager_config)
    baseline_task = registry.create_task(EXTERNAL_MANAGER_TASK_ID, baseline_config)

    assert type(external_task).__module__ == "example_direct_cartpole"
    assert type(external_task).__name__ == "DirectCartPoleTask"
    assert type(manager_task) is DirectTask
    assert external_task.capabilities.train.status is CapabilityStatus.SUPPORTED
    assert external_task.capabilities.evaluate.status is CapabilityStatus.SUPPORTED
    assert external_task.capabilities.export.status is CapabilityStatus.UNSUPPORTED
    assert manager_task.capabilities.train.status.value == "unknown"
    external_result = _run_scripted_trajectory(external_task)
    manager_result = _run_scripted_trajectory(manager_task)
    baseline_result = _run_scripted_trajectory(baseline_task)
    assert manager_task.capabilities.train.status.value == "supported"
    assert manager_task.capabilities.evaluate.status is CapabilityStatus.SUPPORTED
    assert manager_task.capabilities.export.status is CapabilityStatus.SUPPORTED

    _assert_reviewed_behavior(external_result, edited_reward=True)
    _assert_reviewed_behavior(manager_result, edited_reward=True)
    _assert_same_trajectory(external_result, manager_result)
    _assert_reviewed_behavior(baseline_result, edited_reward=False)
    assert external_result.steps[0].rewards[0].item() == pytest.approx(0.975)
    assert baseline_result.steps[0].rewards[0].item() == pytest.approx(0.985)
    assert (
        external_result.steps[0].rewards[0].item()
        - baseline_result.steps[0].rewards[0].item()
    ) == pytest.approx(-0.01)


def test_external_direct_task_handles_worker_scalar_fields_and_sparse_reset() -> None:
    """Actual Worker scalar fields have shape (slots,), including reset states."""
    task = _create_external_minimal_task(_external_config())
    result = _run_scripted_trajectory(task, scalar_fields=True)
    _assert_reviewed_behavior(result, edited_reward=True)

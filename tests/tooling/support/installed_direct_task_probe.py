"""Run installed Direct Task wheels through the shared DirectEnv lifecycle."""

from __future__ import annotations

import importlib
import sys
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any, TypeVar, cast

import torch

_INSTALLED_TARGET = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(_INSTALLED_TARGET))

import uerl  # noqa: E402
from uerl import (  # noqa: E402
    ConstraintFrame,
    InitialState,
    JointTopology,
    ObsType,
    PhysicalCommandBatch,
    PostResetState,
    RobotSpec,
    RobotTopology,
    SessionSchema,
    StateBatch,
    TransitionState,
    UERLDirectEnv,
    merge_robot_spec,
    robot_actuator_action_schema,
    robot_observation_schema,
)
from uerl.assets.robots.cartpole import CARTPOLE_CFG  # noqa: E402
from uerl.core.direct.capabilities import CapabilityStatus  # noqa: E402
from uerl.core.direct.robot_observation import ObservationShapeTable  # noqa: E402
from uerl.core.direct.task import DirectTask  # noqa: E402
from uerl.core.mdp.lib.events import EventEffect  # noqa: E402
from uerl.tasks.registry import create_default_registry  # noqa: E402

_STATE_FIELDS = (
    "robot.joint.pole.joint_position",
    "robot.joint.pole.joint_velocity",
    "robot.joint.cart.joint_position",
    "robot.joint.cart.joint_velocity",
)
_OBSERVATION_SHAPES = ObservationShapeTable({ObsType.JOINT_POSITION: (1,), ObsType.JOINT_VELOCITY: (1,)})
_StateBatchT = TypeVar("_StateBatchT", bound=StateBatch)


def _robot_spec() -> RobotSpec:
    frame = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))
    topology = RobotTopology(
        body_names=("base", "cart", "pole"),
        body_motion_types=("kinematic", "simulated", "simulated"),
        root_body_index=0,
        fixed_base=True,
        joints=(
            JointTopology(
                "cart",
                0,
                1,
                1,
                "linear_x",
                "prismatic",
                "m",
                -1.0,
                1.0,
                frame,
                frame,
                0.0,
            ),
            JointTopology(
                "pole",
                1,
                2,
                1,
                "twist",
                "revolute",
                "rad",
                -3.14,
                3.14,
                frame,
                frame,
                0.0,
            ),
        ),
    )
    return merge_robot_spec(CARTPOLE_CFG.to_robot_config(), topology)


class _ScriptedSession:
    """Supply one deterministic CartPole transition to the installed Task."""

    num_slots = 1

    def __init__(self, robot_spec: RobotSpec) -> None:
        self.robot_spec = robot_spec
        self.reset_masks: list[torch.Tensor] = []
        self.step_command: torch.Tensor | None = None
        self.close_calls = 0

    @property
    def descriptor(self) -> Mapping[str, Any]:
        return {
            "available_state_schema": list(robot_observation_schema(self.robot_spec, _OBSERVATION_SHAPES)),
            "available_action_schema": list(robot_actuator_action_schema(self.robot_spec)),
        }

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        return {}

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        return {}

    def initialize_with_robot_spec(
        self,
        schema: SessionSchema,
        bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
    ) -> InitialState:
        del schema
        bind_robot_spec(self.robot_spec, _OBSERVATION_SHAPES)
        return _state_batch(InitialState, (0.0, 0.0, 0.0, 0.0))

    def acknowledge_ready(self) -> None:
        pass

    def reset(
        self,
        reset_mask: torch.Tensor,
        terrain_levels: torch.Tensor | None = None,
        reset_values: torch.Tensor | None = None,
    ) -> PostResetState:
        del terrain_levels, reset_values
        self.reset_masks.append(reset_mask.clone())
        return _state_batch(PostResetState, (0.0, 0.0, 0.0, 0.0))

    def step(self, command: PhysicalCommandBatch, step_decimation: int | None = None) -> TransitionState:
        assert step_decimation == 1
        self.step_command = command["robot.actuator.target"].clone()
        return _state_batch(TransitionState, (0.1, 0.2, 0.3, 0.4))

    def apply_event(self, effect: EventEffect) -> Mapping[str, Any]:
        return {"applied": True, "kind": effect.kind}

    def close(self, reason: str = "session_close") -> None:
        del reason
        self.close_calls += 1


def _state_batch(batch_type: type[_StateBatchT], values: tuple[float, float, float, float]) -> _StateBatchT:
    named_values = {
        name: torch.tensor([value], dtype=torch.float32)
        for name, value in zip(_STATE_FIELDS, values, strict=True)
    }
    return batch_type(
        named_values,
        torch.tensor([True], dtype=torch.bool),
        torch.tensor([0], dtype=torch.uint16),
        torch.tensor([0], dtype=torch.uint64),
    )


def _verify_installed_task(task_id: str) -> None:
    registry = create_default_registry()
    config = registry.create_task_config(task_id)
    task = registry.create_task(task_id, config)
    assert isinstance(task, DirectTask)
    assert task.capabilities.train.status is CapabilityStatus.SUPPORTED
    assert task.capabilities.evaluate.status is CapabilityStatus.SUPPORTED
    assert task.capabilities.export.status is CapabilityStatus.UNSUPPORTED

    module = importlib.import_module(type(task).__module__)
    assert module.__file__ is not None
    assert Path(module.__file__).resolve().is_relative_to(_INSTALLED_TARGET)

    session = _ScriptedSession(_robot_spec())
    env = UERLDirectEnv(session, task)
    try:
        torch.testing.assert_close(env.get_observations()["policy"], torch.zeros((1, 4)))
        observations, rewards, terminated, truncated, info = env.step(torch.tensor([[0.25]]))
        torch.testing.assert_close(
            observations["policy"], torch.tensor([[0.1, 0.2, 0.3, 0.4]]), atol=1e-6, rtol=0.0
        )
        torch.testing.assert_close(rewards, torch.tensor([0.975]), atol=1e-6, rtol=0.0)
        assert terminated.tolist() == [False]
        assert truncated.tolist() == [False]
        terminal_valid = cast(torch.Tensor, info["terminal_observation_valid"])
        assert terminal_valid.tolist() == [True]
        assert session.step_command is not None
        torch.testing.assert_close(session.step_command, torch.tensor([[25.0]]))
        assert session.reset_masks[0].tolist() == [True]
    finally:
        env.close()
    assert session.close_calls == 1


def main() -> None:
    assert Path(uerl.__file__).resolve().is_relative_to(_INSTALLED_TARGET)
    import os

    os.environ["UERL_TASK_PLUGINS"] = "example-direct-cartpole,direct-balance-demo"
    _verify_installed_task("UERL-DirectCartPole-v0")
    _verify_installed_task("UERL-DirectBalanceDemo-v0")
    print("[PASS] installed Direct Tasks stepped through DirectEnv")


if __name__ == "__main__":
    main()

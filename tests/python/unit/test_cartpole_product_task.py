"""Verify formal Cart-Pole Task math without UE, Socket, or RSL-RL.

Assertion对照 (ticket 26 — same numeric expectations as pre-migration):
- action 0.5 → target 50.0; clip ±1 then ×100 → ±100 / 25
- action_clip=0.5 with input 2.0 → target 50.0
- policy obs order pole_pos, pole_vel, cart_pos, cart_vel; values [[1,3,0,0],[2,4,0,0]]
- bounds: cart==3 / pole==π/2 not terminated; +eps is; no truncated
- timeout at episode_steps == max_episode_steps-1 only
- rewards alive [1.0, -0.04]; terminal [-2.0, -0.04]
"""

from __future__ import annotations

import math
from collections.abc import Mapping
from typing import Any

import pytest
import torch

from uerl import (
    ConfigError,
    DirectTaskConfig,
    InitialState,
    ObservationShapeTable,
    ObsType,
    StepContext,
    TerminationResult,
    UERLDirectEnv,
    robot_actuator_action_schema,
    robot_observation_schema,
)
from uerl.core.config.robot import RobotSpec, RobotTopology, merge_robot_spec, parse_robot_config
from uerl.core.direct.task import DirectTask
from uerl.tasks.cartpole import (
    CARTPOLE_CONTROL_DT,
    CARTPOLE_TASK_ID,
    CartPoleTaskConfig,
    build_cartpole_composed_cfg,
    create_cartpole_task,
)

CARTPOLE_SHAPES = ObservationShapeTable(
    {
        ObsType.JOINT_POSITION: (),
        ObsType.JOINT_VELOCITY: (),
    }
)


def _robot_spec() -> RobotSpec:
    config = parse_robot_config(
        """
actuators:
  - joint: cart
    stiffness: 0.0
    damping: 1.0
    effort_limit: 100.0
    default_pos: 0.0
    action_scale: 100.0
observations:
  - type: joint_position
    joint: pole
  - type: joint_velocity
    joint: pole
  - type: joint_position
    joint: cart
  - type: joint_velocity
    joint: cart
reset:
  distributions: []
"""
    )
    topology = RobotTopology.from_response(
        {
            "asset_path": "/Game/Robots/CartPole/SKM_CartPole",
            "body_names": ["base", "cart", "pole"],
            "body_motion_types": ["kinematic", "simulated", "simulated"],
            "root_body_index": 0,
            "fixed_base": True,
            "joints": [
                {
                    "name": "cart",
                    "parent_body_index": 0,
                    "child_body_index": 1,
                    "degrees_of_freedom": 1,
                    "coordinate": "linear_x",
                    "coordinate_type": "prismatic",
                    "unit": "m",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": {"position_metres": [0.0, 0.0, 0.0], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
                    "parent_frame": {"position_metres": [0.0, 0.0, 0.0], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
                },
                {
                    "name": "pole",
                    "parent_body_index": 1,
                    "child_body_index": 2,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -3.14,
                    "upper_limit": 3.14,
                    "child_frame": {"position_metres": [0.0, 0.0, 0.1], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
                    "parent_frame": {"position_metres": [0.0, 0.0, 0.1], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
                },
            ],
        }
    )
    return merge_robot_spec(config, topology)


def _make_task(config: CartPoleTaskConfig | DirectTaskConfig | None = None) -> DirectTask:
    return create_cartpole_task(
        CartPoleTaskConfig() if config is None else config,
        robot_spec=_robot_spec(),
        observation_shapes=CARTPOLE_SHAPES,
    )


def _context(
    task: DirectTask,
    state: torch.Tensor,
    episode_steps: torch.Tensor | None = None,
) -> StepContext:
    """Build a compact valid-row context from the Cart-Pole observation order."""

    fields = task.observation_groups["policy"]
    values = {name: state[:, index : index + 1] for index, name in enumerate(fields)}
    steps = episode_steps if episode_steps is not None else torch.zeros(state.shape[0], dtype=torch.long)
    command = task.preprocess_actions(torch.zeros(state.shape[0], 1), values)
    return StepContext(
        raw_state=values,
        previous_policy_actions=torch.zeros(state.shape[0], 1),
        policy_actions=torch.zeros(state.shape[0], 1),
        physical_command=command,
        transition_state=values,
        state_valid=torch.ones(state.shape[0], dtype=torch.bool),
        slot_fault_code=torch.zeros(state.shape[0], dtype=torch.uint16),
        episode_steps=steps,
    )


class _ReorderedObservationTask(DirectTask):
    @property
    def observation_groups(self) -> Mapping[str, tuple[str, ...]]:
        fields = super().observation_groups["policy"]
        return {"policy": tuple(reversed(fields))}


class _RobotSpecBindingSession:
    num_slots = 1

    def __init__(self) -> None:
        self.close_calls = 0
        spec = _robot_spec()
        self.descriptor = {
            "available_state_schema": list(robot_observation_schema(spec, CARTPOLE_SHAPES)),
            "available_action_schema": list(robot_actuator_action_schema(spec)),
        }

    def initialize_with_robot_spec(self, schema: Any, bind_robot_spec: Any) -> InitialState:
        del schema
        spec = _robot_spec()
        bound_schema = bind_robot_spec(spec, CARTPOLE_SHAPES)
        names = tuple(str(item["name"]) for item in bound_schema.as_request()["state_requirements"])
        return InitialState(
            {name: torch.zeros(1) for name in names},
            torch.ones(1, dtype=torch.bool),
            torch.zeros(1, dtype=torch.uint16),
            torch.zeros(1, dtype=torch.uint64),
        )

    def acknowledge_ready(self) -> None:
        pass

    def close(self, reason: str = "session_close") -> None:
        del reason
        self.close_calls += 1


def test_cartpole_robot_spec_drives_action_schema_and_scale() -> None:
    task = _make_task(CartPoleTaskConfig())
    commands = task.preprocess_actions(torch.tensor([[0.5]]), {})

    torch.testing.assert_close(commands["robot.actuator.target"], torch.tensor([[50.0]]))
    assert task.schema.as_request()["action_schema"][0]["shape"] == [1]
    assert task.observation_groups["policy"] == (
        "robot.joint.pole.joint_position",
        "robot.joint.pole.joint_velocity",
        "robot.joint.cart.joint_position",
        "robot.joint.cart.joint_velocity",
    )


def test_ac_py_unit_generic_001_direct_env_rejects_task_reordering_of_robot_projection() -> None:
    params = CartPoleTaskConfig()
    task = _ReorderedObservationTask(
        control_dt=CARTPOLE_CONTROL_DT,
        batch_size=1,
        device="cpu",
        cfg_factory=lambda spec: build_cartpole_composed_cfg(params, spec),
        task_config=params,
    )
    session = _RobotSpecBindingSession()

    with pytest.raises(ConfigError) as raised:
        UERLDirectEnv(session, task)  # type: ignore[arg-type]

    assert raised.value.code == "ROBOT_OBSERVATION_PROJECTION_MISMATCH"
    assert session.close_calls == 1


def test_cartpole_robot_observation_values_follow_declared_order() -> None:
    task = _make_task(CartPoleTaskConfig())
    raw_state = {
        "robot.joint.pole.joint_position": torch.tensor([[1.0], [2.0]]),
        "robot.joint.pole.joint_velocity": torch.tensor([[3.0], [4.0]]),
        "robot.joint.cart.joint_position": torch.zeros(2, 1),
        "robot.joint.cart.joint_velocity": torch.zeros(2, 1),
    }

    observations = task.build_observations(
        raw_state, torch.ones(2, dtype=torch.bool), torch.zeros(2, 1)
    )

    torch.testing.assert_close(observations["policy"], torch.tensor([[1.0, 3.0, 0.0, 0.0], [2.0, 4.0, 0.0, 0.0]]))


def test_ac_py_unit_generic_002_cartpole_schema_and_robot_action_scale_are_stable() -> None:
    """Verify names, units, clipping, and Newton action scaling."""

    task = _make_task()
    commands = task.preprocess_actions(torch.tensor([[-2.0], [0.25], [2.0]]), {})

    assert task.schema.as_request()["state_requirements"]
    assert [item["name"] for item in task.schema.as_request()["state_requirements"]] == [
        "robot.joint.pole.joint_position",
        "robot.joint.pole.joint_velocity",
        "robot.joint.cart.joint_position",
        "robot.joint.cart.joint_velocity",
    ]
    assert commands["robot.actuator.target"].shape == (3, 1)
    torch.testing.assert_close(commands["robot.actuator.target"], torch.tensor([[-100.0], [25.0], [100.0]]))


def test_cartpole_bounds_trigger_only_physical_termination() -> None:
    """Verify strict cart and pole thresholds without a timeout."""

    state = torch.zeros(4, 4)
    state[0, 2] = 3.0
    state[1, 2] = 3.0 + 1.0e-3
    state[2, 0] = math.pi / 2
    state[3, 0] = math.pi / 2 + 1.0e-3

    task = _make_task()
    terminations = task.compute_terminations(_context(task, state))

    assert not bool(terminations.terminated[0])
    assert bool(terminations.terminated[1])
    assert not bool(terminations.terminated[2])
    assert bool(terminations.terminated[3])
    assert not bool(terminations.truncated.any())


def test_cartpole_timeout_is_separate_from_termination() -> None:
    """Verify the configured episode limit produces truncation only."""

    task = _make_task()
    state = torch.zeros(2, 4)
    episode_steps = torch.tensor([task.max_episode_steps - 1, 1], dtype=torch.long)

    terminations = task.compute_terminations(_context(task, state, episode_steps))

    assert not bool(terminations.terminated.any())
    assert bool(terminations.truncated[0])
    assert not bool(terminations.truncated[1])


def test_cartpole_reward_uses_pre_reset_transition_state() -> None:
    """Verify alive, terminal, and shaping terms from the transition batch."""

    task = _make_task()
    state = torch.tensor([[0.0, 0.0, 0.0, 0.0], [1.0, 4.0, 0.0, 2.0]])
    context = _context(task, state)

    alive = task.compute_rewards(
        context,
        TerminationResult(torch.zeros(2, dtype=torch.bool), torch.zeros(2, dtype=torch.bool)),
    )
    terminal = task.compute_rewards(
        context,
        TerminationResult(torch.tensor([True, False]), torch.zeros(2, dtype=torch.bool)),
    )

    torch.testing.assert_close(alive, torch.tensor([1.0, -0.04]))
    torch.testing.assert_close(terminal, torch.tensor([-2.0, -0.04]))
    print(f"[VERIFY] VC-007: task_id={CARTPOLE_TASK_ID} math=PASS")


def test_cartpole_config_factory_preserves_typed_parameters() -> None:
    """Verify per-Run config parameters remain typed and immutable."""

    config = CartPoleTaskConfig(action_clip=0.5)
    task = _make_task(config)
    commands = task.preprocess_actions(torch.tensor([[2.0]]), {})

    assert isinstance(task.config, CartPoleTaskConfig)
    torch.testing.assert_close(commands["robot.actuator.target"], torch.tensor([[50.0]]))


def test_cartpole_generic_config_preserves_explicit_zero_timeout() -> None:
    """Preserve the Direct Config meaning of zero: no timeout."""

    task = _make_task(DirectTaskConfig(max_episode_steps=0))

    assert task.max_episode_steps == 0

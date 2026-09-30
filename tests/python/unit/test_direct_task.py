"""Verify DirectTask manager assembly order and empty command seam."""

from __future__ import annotations

import pytest
import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import ObsType, RobotSpec, RobotTopology, merge_robot_spec, parse_robot_config
from uerl.core.direct.robot_observation import ObservationShapeTable
from uerl.core.direct.task import DirectTask, DirectTaskCfg, EmptyCommandSource
from uerl.core.direct.types import StepContext, TerminationResult
from uerl.core.mdp import lib as mdp
from uerl.core.mdp.terms import (
    ActionCfg,
    ActionTermCfg,
    DoneTermCfg,
    ObservationCfg,
    ObsGroupCfg,
    ObsTermCfg,
    RewardCfg,
    RewTermCfg,
    TerminationCfg,
)
from uerl.errors import ConfigError
from uerl.tasks.cartpole import CartPoleTaskConfig, create_cartpole_task

SCALAR_SHAPES = ObservationShapeTable(
    {
        ObsType.JOINT_POSITION: (),
        ObsType.JOINT_VELOCITY: (),
    }
)


def _two_joint_spec() -> RobotSpec:
    config = parse_robot_config(
        """
actuators:
  - joint: hip
    stiffness: 0.0
    damping: 0.1
    effort_limit: 1.0
    default_pos: 0.0
    action_scale: 2.0
observations:
  - type: joint_position
    joint: hip
  - type: joint_velocity
    joint: hip
reset:
  distributions: []
"""
    )
    frame = {
        "position_metres": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    topology = RobotTopology.from_response(
        {
            "asset_path": "/Game/Test/SK_Test",
            "body_names": ["base", "thigh"],
            "body_motion_types": ["simulated", "simulated"],
            "root_body_index": 0,
            "fixed_base": False,
            "joints": [
                {
                    "name": "hip",
                    "parent_body_index": 0,
                    "child_body_index": 1,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": frame,
                    "parent_frame": frame,
                },
            ],
        }
    )
    return merge_robot_spec(config, topology)


def test_ac_py_unit_composed_001_implements_direct_task_contract() -> None:
    task = create_cartpole_task(CartPoleTaskConfig())
    assert isinstance(task, DirectTask)
    assert callable(task.preprocess_actions)
    assert callable(task.build_observations)
    assert callable(task.compute_terminations)
    assert callable(task.compute_rewards)
    assert hasattr(task, "observation_groups")
    assert hasattr(task, "schema")


def test_ac_py_unit_composed_002_assembly_order_obs_and_reward_depend_on_prior() -> None:
    """Observation references previous_action width; reward inspects terminations."""

    spec = _two_joint_spec()
    cfg = DirectTaskCfg(
        task=CartPoleTaskConfig(max_episode_steps=10),
        actions=ActionCfg(
            terms={
                "hip": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="hip"),
                    target_mode="effort",
                    scale=2.0,
                    use_default_offset=False,
                )
            }
        ),
        observations=ObservationCfg(
            groups={
                "policy": ObsGroupCfg(
                    terms={
                        "prev": ObsTermCfg(op="previous_action", params={"width": 1}),
                        "hip": ObsTermCfg(
                            op="select",
                            params={
                                "entity": RobotEntityCfg(joint_names="hip"),
                                "field": "joint_position",
                            },
                        ),
                    }
                )
            }
        ),
        terminations=TerminationCfg(
            terms={
                "time_out": DoneTermCfg(
                    func=mdp.terminations.time_out,
                    time_out=True,
                    params={"max_steps": 9},
                )
            }
        ),
        rewards=RewardCfg(
            terms={
                "alive": RewTermCfg(func=mdp.rewards.is_alive, weight=1.0, consumes_terminations=True),
                "dead": RewTermCfg(func=mdp.rewards.is_terminated, weight=-2.0, consumes_terminations=True),
            }
        ),
    )
    task = DirectTask(
        cfg,
        robot_spec=spec,
        observation_shapes=SCALAR_SHAPES,
        control_dt=1.0 / 60.0,
        batch_size=2,
        device="cpu",
    )
    assert task.num_actions == 1
    assert task.observation_plan.group_widths["policy"] == 2

    prev = torch.tensor([[0.25], [-0.5]])
    obs = task.build_observations(
        {
            "robot.joint.hip.joint_position": torch.zeros(2, 1),
            "robot.joint.hip.joint_velocity": torch.zeros(2, 1),
        },
        torch.ones(2, dtype=torch.bool),
        prev,
    )
    torch.testing.assert_close(obs["policy"][:, 0:1], prev)

    ctx = StepContext(
        raw_state={},
        previous_policy_actions=prev,
        policy_actions=prev,
        physical_command=task.preprocess_actions(prev, {}),
        transition_state={
            "robot.joint.hip.joint_position": torch.zeros(2, 1),
            "robot.joint.hip.joint_velocity": torch.zeros(2, 1),
        },
        state_valid=torch.ones(2, dtype=torch.bool),
        slot_fault_code=torch.zeros(2, dtype=torch.uint16),
        episode_steps=torch.zeros(2, dtype=torch.long),
    )
    rewards = task.compute_rewards(
        ctx,
        TerminationResult(torch.tensor([True, False]), torch.zeros(2, dtype=torch.bool)),
    )
    torch.testing.assert_close(rewards, torch.tensor([-2.0, 1.0]))
    assert task.episode_reward_log(torch.tensor([True, False])) == {
        "Episode_Reward/alive": 0.0,
        "Episode_Reward/dead": -2.0,
    }
    task.on_reset(torch.tensor([True, False]), {})
    assert task.episode_reward_log(torch.tensor([True, False])) == {
        "Episode_Reward/alive": 0.0,
        "Episode_Reward/dead": 0.0,
    }
    assert task.episode_reward_log(torch.tensor([False, True])) == {
        "Episode_Reward/alive": 1.0,
        "Episode_Reward/dead": 0.0,
    }


def test_ac_py_unit_composed_003_empty_command_source_rejects_command_op() -> None:
    spec = _two_joint_spec()
    cfg = DirectTaskCfg(
        task=CartPoleTaskConfig(max_episode_steps=0),
        actions=ActionCfg(
            terms={
                "hip": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="hip"),
                    target_mode="effort",
                    use_default_offset=False,
                )
            }
        ),
        observations=ObservationCfg(
            groups={
                "policy": ObsGroupCfg(
                    terms={
                        "cmd": ObsTermCfg(
                            op="command",
                            params={"channel": "velocity", "width": 3},
                        )
                    }
                )
            }
        ),
        terminations=TerminationCfg(
            terms={
                "never": DoneTermCfg(
                    func=mdp.terminations.time_out,
                    time_out=True,
                    params={"max_steps": 10**9},
                )
            }
        ),
        rewards=RewardCfg(
            terms={
                "alive": RewTermCfg(
                    func=mdp.rewards.is_alive,
                    weight=1.0,
                    consumes_terminations=True,
                )
            }
        ),
    )
    with pytest.raises(ConfigError) as raised:
        DirectTask(
            cfg,
            robot_spec=spec,
            observation_shapes=SCALAR_SHAPES,
            control_dt=1.0 / 60.0,
            batch_size=1,
            device="cpu",
            command_source=EmptyCommandSource(),
        )
    assert raised.value.code == "OP_MISSING_CHANNEL"


def test_ac_py_unit_composed_004_cartpole_numerical_parity() -> None:
    """Frozen expectations from pre-migration CartPoleTask (tolerance 1e-6)."""

    import math

    from tests.python.unit.test_cartpole_product_task import _robot_spec

    task = create_cartpole_task(CartPoleTaskConfig())
    task.bind_robot_spec(_robot_spec(), observation_shapes=SCALAR_SHAPES)
    commands = task.preprocess_actions(torch.tensor([[0.5], [-1.0], [1.0]]), {})
    torch.testing.assert_close(
        commands["robot.actuator.target"],
        torch.tensor([[50.0], [-100.0], [100.0]]),
        atol=1e-6,
        rtol=0.0,
    )
    raw = {
        "robot.joint.pole.joint_position": torch.tensor([[0.1]]),
        "robot.joint.pole.joint_velocity": torch.tensor([[0.2]]),
        "robot.joint.cart.joint_position": torch.tensor([[0.3]]),
        "robot.joint.cart.joint_velocity": torch.tensor([[0.4]]),
    }
    obs = task.build_observations(raw, torch.ones(1, dtype=torch.bool), torch.zeros(1, 1))
    torch.testing.assert_close(obs["policy"], torch.tensor([[0.1, 0.2, 0.3, 0.4]]), atol=1e-6, rtol=0.0)

    state = torch.tensor([[0.0, 0.0, 0.0, 0.0], [1.0, 4.0, 0.0, 2.0]])
    fields = task.observation_groups["policy"]
    values = {name: state[:, index : index + 1] for index, name in enumerate(fields)}
    ctx = StepContext(
        raw_state=values,
        previous_policy_actions=torch.zeros(2, 1),
        policy_actions=torch.zeros(2, 1),
        physical_command=task.preprocess_actions(torch.zeros(2, 1), values),
        transition_state=values,
        state_valid=torch.ones(2, dtype=torch.bool),
        slot_fault_code=torch.zeros(2, dtype=torch.uint16),
        episode_steps=torch.zeros(2, dtype=torch.long),
    )
    alive = task.compute_rewards(
        ctx,
        TerminationResult(torch.zeros(2, dtype=torch.bool), torch.zeros(2, dtype=torch.bool)),
    )
    torch.testing.assert_close(alive, torch.tensor([1.0, -0.04]), atol=1e-6, rtol=0.0)
    terminal = task.compute_rewards(
        ctx,
        TerminationResult(torch.tensor([True, False]), torch.zeros(2, dtype=torch.bool)),
    )
    torch.testing.assert_close(terminal, torch.tensor([-2.0, -0.04]), atol=1e-6, rtol=0.0)

    bound = torch.zeros(4, 4)
    bound[0, 2] = 3.0
    bound[1, 2] = 3.0 + 1.0e-3
    bound[2, 0] = math.pi / 2
    bound[3, 0] = math.pi / 2 + 1.0e-3
    bound_values = {name: bound[:, index : index + 1] for index, name in enumerate(fields)}
    bound_ctx = StepContext(
        raw_state=bound_values,
        previous_policy_actions=torch.zeros(4, 1),
        policy_actions=torch.zeros(4, 1),
        physical_command=task.preprocess_actions(torch.zeros(4, 1), bound_values),
        transition_state=bound_values,
        state_valid=torch.ones(4, dtype=torch.bool),
        slot_fault_code=torch.zeros(4, dtype=torch.uint16),
        episode_steps=torch.zeros(4, dtype=torch.long),
    )
    terminations = task.compute_terminations(bound_ctx)
    assert not bool(terminations.terminated[0])
    assert bool(terminations.terminated[1])
    assert not bool(terminations.terminated[2])
    assert bool(terminations.terminated[3])
    assert not bool(terminations.truncated.any())

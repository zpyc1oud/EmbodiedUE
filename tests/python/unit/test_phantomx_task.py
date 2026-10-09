"""Verify PhantomX uses the Generic Robot runtime and task-owned locomotion math."""

from __future__ import annotations

import math
from dataclasses import replace
from pathlib import Path
from typing import cast

import pytest
import torch

from tests.python.unit.robot_shape_fixtures import generic_robot_observation_shapes
from uerl.assets.robots.phantomx import PHANTOMX_CFG
from uerl.core.config.robot import ObsType, RobotSpec, RobotTopology, merge_robot_spec
from uerl.core.direct.capabilities import CapabilityStatus
from uerl.core.direct.robot_action import ROBOT_ACTUATOR_TARGET_FIELD
from uerl.core.direct.robot_observation import robot_observation_schema
from uerl.core.direct.types import PhysicalCommandBatch, StepContext, TerminationResult
from uerl.core.mdp.executor import PlanExecutor, PlanInputs
from uerl.core.mdp.plan import ObservationPlan
from uerl.errors import ConfigError
from uerl.tasks.phantomx.commands import PhantomXVelocityCommandSource
from uerl.tasks.phantomx.config import (
    PHANTOMX_COMMAND_FIELDS,
    PHANTOMX_ENVIRONMENT_ID,
    PHANTOMX_JOINTS,
    PHANTOMX_ROBOT_ID,
    PhantomXCommandConfig,
    PhantomXTaskConfig,
    load_phantomx_training_config,
    parse_phantomx_training_config,
)
from uerl.tasks.phantomx.pursuit import PHANTOMX_PURSUIT_TARGET_FIELD
from uerl.tasks.phantomx.task import PhantomXTask, create_phantomx_pursuit_direct_task

ROOT = Path(__file__).resolve().parents[3]
TRAINING_PATH = ROOT / "src" / "uerl" / "configs" / "tasks" / "phantomx" / "training.yaml"
SHAPES = generic_robot_observation_shapes()
TERRAIN_SHAPE = SHAPES.shape_for(ObsType.TERRAIN_HEIGHT)


def _robot_spec() -> RobotSpec:
    training = load_phantomx_training_config(TRAINING_PATH)
    # The UE PhantomX PhysicsAsset exposes the joint-named distal bodies
    # directly (including the six tibia feet).
    body_names = ["base_link", *PHANTOMX_JOINTS]
    joints: list[dict[str, object]] = []
    for index, joint in enumerate(PHANTOMX_JOINTS):
        leg_offset = index - index % 3
        parent_body_index = 0 if index % 3 == 0 else index
        child_body_index = index + 1
        joints.append(
            {
                "name": joint,
                "parent_body_index": parent_body_index,
                "child_body_index": child_body_index,
                "degrees_of_freedom": 1,
                "coordinate": "twist",
                "coordinate_type": "revolute",
                "unit": "rad",
                "default_position": 0.0,
                "lower_limit": -3.141592653589793,
                "upper_limit": 3.141592653589793,
                "child_frame": {
                    "position_metres": [0.0, 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "parent_frame": {
                    "position_metres": [float(leg_offset), 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            }
        )
    topology = RobotTopology.from_response(
        {
            "asset_path": training.robot_asset_path,
            "body_names": body_names,
            "body_motion_types": ["simulated"] * len(body_names),
            "root_body_index": 0,
            "fixed_base": False,
            "joints": joints,
        }
    )
    return merge_robot_spec(training.robot_config, topology)


def _task(config: PhantomXTaskConfig | None = None) -> PhantomXTask:
    return PhantomXTask(config, robot_spec=_robot_spec(), observation_shapes=SHAPES)


def _state(
    task: PhantomXTask,
    *,
    num_envs: int = 1,
    joint_position: torch.Tensor | None = None,
    joint_velocity: torch.Tensor | None = None,
    pose: torch.Tensor | None = None,
    linear_velocity: torch.Tensor | None = None,
    angular_velocity: torch.Tensor | None = None,
    ground_clearance: torch.Tensor | None = None,
    terrain_height: torch.Tensor | None = None,
    contact: torch.Tensor | None = None,
    contact_force: torch.Tensor | None = None,
    initial_command: torch.Tensor | None = None,
    heading_command: torch.Tensor | None = None,
    post_turn_command: torch.Tensor | None = None,
    pursuit_target: torch.Tensor | None = None,
) -> dict[str, torch.Tensor]:
    assert task.robot_spec is not None
    spec = task.robot_spec
    joint_position = (
        torch.tensor([actuator.default_pos for actuator in spec.actuators]).repeat(num_envs, 1)
        if joint_position is None
        else joint_position
    )
    joint_velocity = torch.zeros(num_envs, 18) if joint_velocity is None else joint_velocity
    pose = (
        torch.tensor([[0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0]]).repeat(num_envs, 1)
        if pose is None
        else pose
    )
    linear_velocity = torch.zeros(num_envs, 3) if linear_velocity is None else linear_velocity
    angular_velocity = torch.zeros(num_envs, 3) if angular_velocity is None else angular_velocity
    ground_clearance = torch.full((num_envs,), 0.18) if ground_clearance is None else ground_clearance
    terrain_height = torch.zeros(num_envs, *TERRAIN_SHAPE) if terrain_height is None else terrain_height
    contact = torch.zeros(num_envs, 6) if contact is None else contact
    contact_force = torch.zeros(num_envs, 7) if contact_force is None else contact_force
    values: dict[str, torch.Tensor] = {}
    position_column = 0
    velocity_column = 0
    contact_column = 0
    contact_force_column = 0
    for descriptor in robot_observation_schema(spec, SHAPES):
        name = cast(str, descriptor["name"])
        semantic = descriptor["semantic"]
        if semantic == "joint_position":
            values[name] = joint_position[:, position_column]
            position_column += 1
        elif semantic == "joint_velocity":
            values[name] = joint_velocity[:, velocity_column]
            velocity_column += 1
        elif semantic == "body_pose":
            values[name] = pose
        elif semantic == "body_linear_velocity":
            values[name] = linear_velocity
        elif semantic == "body_angular_velocity":
            values[name] = angular_velocity
        elif semantic == "ground_clearance":
            values[name] = ground_clearance
        elif semantic == "terrain_height":
            values[name] = terrain_height
        elif semantic == "contact":
            values[name] = contact[:, contact_column]
            contact_column += 1
        elif semantic == "contact_force":
            values[name] = contact_force[:, contact_force_column]
            contact_force_column += 1
    values[PHANTOMX_COMMAND_FIELDS[0]] = (
        torch.tensor([[0.5, 0.0]]).repeat(num_envs, 1)
        if initial_command is None
        else initial_command
    )
    values[PHANTOMX_COMMAND_FIELDS[1]] = (
        torch.zeros(num_envs, 1) if heading_command is None else heading_command
    )
    values[PHANTOMX_COMMAND_FIELDS[2]] = (
        values[PHANTOMX_COMMAND_FIELDS[0]].clone()
        if post_turn_command is None
        else post_turn_command
    )
    if pursuit_target is not None:
        values[PHANTOMX_PURSUIT_TARGET_FIELD] = pursuit_target
    return values


def _context(
    task: PhantomXTask,
    state: dict[str, torch.Tensor],
    steps: int = 0,
    *,
    previous_state: dict[str, torch.Tensor] | None = None,
) -> StepContext:
    num_envs = next(iter(state.values())).shape[0]
    return StepContext(
        raw_state=state if previous_state is None else previous_state,
        previous_policy_actions=torch.zeros(num_envs, 18),
        policy_actions=torch.zeros(num_envs, 18),
        physical_command=PhysicalCommandBatch(
            {ROBOT_ACTUATOR_TARGET_FIELD: torch.zeros(num_envs, 18)}
        ),
        transition_state=state,
        state_valid=torch.ones(num_envs, dtype=torch.bool),
        slot_fault_code=torch.zeros(num_envs, dtype=torch.int64),
        episode_steps=torch.full((num_envs,), steps, dtype=torch.int64),
        episode_elapsed_s=torch.full((num_envs,), steps * 0.02, dtype=torch.float64),
        transition_dt=0.02,
    )


def test_phantomx_training_config_uses_generic_robot_and_shared_map() -> None:
    config = load_phantomx_training_config(TRAINING_PATH)

    assert len(config.robot_config.actuators) == 18
    assert tuple(actuator.joint for actuator in config.robot_config.actuators) == PHANTOMX_JOINTS
    assert {actuator.effort_limit for actuator in config.robot_config.actuators} == {2.8}
    assert config.worker.robot_id == PHANTOMX_ROBOT_ID == "uerl.robot.skeletal_mesh"
    assert config.worker.environment_id == PHANTOMX_ENVIRONMENT_ID == "uerl.environment.shared_world"
    assert config.worker.robot_semantics is config.robot_config
    assert config.worker.robot_config == {}
    assert config.worker.environment_config["environment.columns"] == 32.0
    assert len(config.robot_config.reset.distributions) == 19
    root_reset = config.robot_config.reset.distributions[0]
    assert root_reset.target_type.value == "root_pose"
    assert root_reset.lower_values == (0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0)
    assert root_reset.upper_values == root_reset.lower_values


def test_phantomx_config_rejects_a_different_asset_contract() -> None:
    text = TRAINING_PATH.read_text(encoding="utf-8")

    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(text.replace("SK_PhantomX.SK_PhantomX", "Other.SK_Other"))

    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert raised.value.path == "robot.asset_path"


def test_phantomx_config_rejects_duplicate_yaml_keys() -> None:
    text = TRAINING_PATH.read_text(encoding="utf-8")

    with pytest.raises(ConfigError) as raised:
        parse_phantomx_training_config(text.replace("  slot_count: 512", "  slot_count: 512\n  slot_count: 32"))

    assert raised.value.code == "CONFIG_INVALID_YAML"


def test_phantomx_task_maps_actions_through_robot_spec() -> None:
    task = _task()
    commands = task.preprocess_actions(torch.tensor([[0.0] * 18, [2.0] * 18]), {})
    defaults = torch.tensor([actuator.default_pos for actuator in cast(RobotSpec, task.robot_spec).actuators])

    assert tuple(commands.values) == (ROBOT_ACTUATOR_TARGET_FIELD,)
    assert torch.allclose(commands[ROBOT_ACTUATOR_TARGET_FIELD][0], defaults)
    assert torch.allclose(commands[ROBOT_ACTUATOR_TARGET_FIELD][1], defaults + 0.20)


def test_phantomx_reset_joint_references_match_actuator_defaults() -> None:
    task = _task()
    spec = cast(RobotSpec, task.robot_spec)
    defaults = {actuator.joint: actuator.default_pos for actuator in spec.actuators}

    for target in spec.reset:
        if target.target_type.value == "joint_position":
            assert target.joint is not None
            assert target.reference == pytest.approx(defaults[target.joint])
            assert target.lower == pytest.approx(0.0)
            assert target.upper == pytest.approx(0.0)


def test_phantomx_session_schema_contains_only_generic_robot_projection() -> None:
    task = _task()
    schema = task.schema.as_request()

    assert [item["name"] for item in schema["action_schema"]] == [ROBOT_ACTUATOR_TARGET_FIELD]
    assert len(schema["state_requirements"]) == 54
    assert all(item["name"].startswith("robot.") for item in schema["state_requirements"])
    assert sum(item["semantic"] == "contact_force" for item in schema["state_requirements"]) == 7
    assert not any(name in {item["name"] for item in schema["state_requirements"]} for name in PHANTOMX_COMMAND_FIELDS)


def test_phantomx_observation_is_body_relative_and_includes_previous_actions() -> None:
    task = _task()
    state = _state(
        task,
        num_envs=2,
        pose=torch.tensor(
            [[0.1, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0], [10.0, -4.0, 0.18, 0.0, 0.0, 0.0, 1.0]]
        ),
        initial_command=torch.tensor([[0.4, -0.2], [0.4, -0.2]]),
    )
    previous_actions = torch.tensor([[0.1] * 18, [-0.2] * 18])

    policy = task.build_observations(state, torch.ones(2, dtype=torch.bool), previous_actions)["policy"]

    assert policy.shape == (2, 109)
    assert torch.allclose(policy[0, :90], policy[1, :90])
    assert torch.allclose(policy[:, 9:12], torch.tensor([[0.4, -0.2, 0.0], [0.4, -0.2, 0.0]]))
    assert torch.equal(policy[:, 90:-1], previous_actions)
    # Direct-call dt fallback is the bootstrap contract: DtMin = 5 ms × scale 100.
    assert torch.equal(policy[:, -1], torch.full((2,), 0.5))


def test_phantomx_policy_receives_terrain_height_and_foot_contact() -> None:
    task = _task()
    terrain_height = (
        torch.arange(2 * TERRAIN_SHAPE[0], dtype=torch.float32)
        .reshape(2, *TERRAIN_SHAPE)
        / 100.0
    )
    contact = torch.tensor(
        [
            [1.0, 0.0, 1.0, 0.0, 1.0, 0.0],
            [0.0, 1.0, 0.0, 1.0, 0.0, 1.0],
        ]
    )
    contact_force = torch.arange(14, dtype=torch.float32).reshape(2, 7)
    state = _state(
        task, num_envs=2, terrain_height=terrain_height, contact=contact, contact_force=contact_force
    )

    observations = task.build_observations(
        state, torch.ones(2, dtype=torch.bool), torch.zeros(2, 18)
    )
    policy = observations["policy"]
    critic = observations["critic"]

    terrain_end = 12 + TERRAIN_SHAPE[0]
    contact_start = terrain_end + 1
    assert torch.allclose(policy[:, 12:terrain_end], terrain_height)
    assert torch.equal(policy[:, contact_start : contact_start + 6], contact)
    assert policy.shape[1] == 109
    assert critic.shape == (2, 116)
    assert torch.equal(critic[:, :109], policy)
    assert torch.equal(critic[:, 109:], contact_force)
    assert not any("contact_force" in name for name in task.observation_plan.state_requirements)


def _policy_velocity(task: PhantomXTask, state: dict[str, torch.Tensor]) -> torch.Tensor:
    return task.build_observations(state, torch.ones(1, dtype=torch.bool), torch.zeros(1, 18))["policy"][
        :, 9:12
    ]


def test_player_keys_replace_the_walk_command_in_the_policy_observation() -> None:
    from uerl.tasks.controllers import PlayerVelocityController

    task = PhantomXTask(
        PhantomXTaskConfig(),
        control_dt=0.02,
        batch_size=1,
        device="cpu",
    )
    player = PlayerVelocityController(speed_mps=0.5, max_yaw_rate=1.0, batch_size=1)
    task.use_command_source(player)
    task.bind_robot_spec(_robot_spec(), observation_shapes=SHAPES)
    state = _state(task, initial_command=torch.tensor([[0.1, 0.2]]))
    cases: tuple[tuple[set[str], list[float]], ...] = (
        ({"W", "D", "E"}, [0.5, 0.0, -1.0]),
        ({"W", "A", "Q"}, [0.5, 0.0, 1.0]),
        ({"W", "S", "A", "D", "Q", "E"}, [0.0, 0.0, 0.0]),
        ({"S", "A", "Q"}, [0.0, 0.0, 0.0]),
        ({"Q"}, [0.0, 0.0, 0.0]),
        (set(), [0.0, 0.0, 0.0]),
    )

    for held, expected in cases:
        player.set_held(held)
        command = _policy_velocity(task, state)
        assert torch.allclose(command[0], torch.tensor(expected), atol=1e-6)
    task.on_reset(torch.ones(1, dtype=torch.bool), state)
    assert torch.allclose(_policy_velocity(task, state), command)


def test_align_batch_lets_reset_use_the_session_slot_count() -> None:
    task = PhantomXTask(PhantomXTaskConfig(), control_dt=0.02, batch_size=4, device="cpu")
    task.align_batch(1)

    task.on_reset(
        torch.ones(1, dtype=torch.bool),
        {"robot.body.base_link.body_pose": torch.tensor([[0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0]])},
    )
    velocity = task.published_velocity()

    assert velocity is not None
    assert velocity.shape == (1, 3)


def test_published_planar_command_is_the_terrain_curriculum_command() -> None:
    from uerl.core.direct.env import _planar_command

    task = _task()
    state = _state(task, initial_command=torch.tensor([[0.6, 0.8]]))
    task.refresh_command(state)
    planar = task.command_planar_speed()

    assert planar is not None
    assert torch.allclose(planar, torch.tensor([1.0]))
    command = _planar_command(task.published_velocity(), 1, torch.device("cpu"))
    assert torch.allclose(command, torch.tensor([[0.6, 0.8]]))


def test_phantomx_turn_waits_for_progress_then_uses_post_turn_speed() -> None:
    task = _task(
        replace(PhantomXTaskConfig(), turn_start_distance=0.5, turn_completion_tolerance=0.1)
    )
    pose = torch.tensor(
        [
            [0.49, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0],
            [0.51, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0],
            [0.51, 0.0, 0.18, 0.0, 0.0, torch.sin(torch.tensor(0.5)), torch.cos(torch.tensor(0.5))],
        ]
    )
    state = _state(
        task,
        num_envs=3,
        pose=pose,
        initial_command=torch.tensor([[0.4, 0.0]] * 3),
        heading_command=torch.ones(3, 1),
        post_turn_command=torch.tensor([[0.8, 0.0]] * 3),
    )

    command = task.build_observations(state, torch.ones(3, dtype=torch.bool), torch.zeros(3, 18))["policy"][:, 9:12]

    assert torch.allclose(command[0], torch.tensor([0.4, 0.0, 0.0]), atol=1e-6)
    assert torch.allclose(command[1], torch.tensor([0.4, 0.0, 0.5]), atol=1e-6)
    assert torch.allclose(command[2], torch.tensor([0.8, 0.0, 0.0]), atol=1e-6)


def test_phantomx_pursuit_preserves_policy_width_and_steers_toward_player() -> None:
    task = create_phantomx_pursuit_direct_task(
        PhantomXTaskConfig(),
        robot_spec=_robot_spec(),
        observation_shapes=SHAPES,
        control_dt=0.02,
        batch_size=3,
    )
    state = _state(
        task,
        num_envs=3,
        pursuit_target=torch.tensor(
            [
                [2.0, 0.0, 0.0],
                [0.0, 2.0, 0.0],
                [1.0, 0.0, 0.0],
            ]
        ),
    )

    policy = task.build_observations(
        state, torch.ones(3, dtype=torch.bool), torch.zeros(3, 18)
    )["policy"]

    assert policy.shape == (3, 109)
    assert torch.allclose(policy[0, 9:12], torch.tensor([0.45, 0.0, 0.0]), atol=1e-6)
    assert torch.allclose(
        policy[1, 9:12], torch.tensor([0.45, 0.0, torch.pi / 4]), atol=1e-6
    )
    assert torch.allclose(policy[2, 9:12], torch.zeros(3), atol=1e-6)
    assert task.schema.state_requirements[-1]["name"] == PHANTOMX_PURSUIT_TARGET_FIELD


def test_phantomx_base_contact_force_terminates() -> None:
    task = _task()
    contact_force = torch.tensor(
        [[task.phantomx_config.base_contact_force_threshold + 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]]
    )
    result = task.compute_terminations(_context(task, _state(task, contact_force=contact_force), 1))

    assert result.terminated.tolist() == [True]
    assert result.truncated.tolist() == [False]
    assert result.reason["base_contact"].tolist() == [True]
    assert set(result.reason) == {"base_contact", "timeout"}


def test_phantomx_orientation_alone_does_not_terminate() -> None:
    task = _task()
    tilted = _state(task, pose=torch.tensor([[0.0, 0.0, 0.18, 0.7, 0.0, 0.0, 0.7]]))
    result = task.compute_terminations(_context(task, tilted, 1))

    assert result.terminated.tolist() == [False]
    assert result.reason["base_contact"].tolist() == [False]


def test_phantomx_timeout_only_truncates() -> None:
    task = _task()
    nominal = _state(task, initial_command=torch.tensor([[0.0, 0.0]]))
    result = task.compute_terminations(_context(task, nominal, 1000))

    assert result.terminated.tolist() == [False]
    assert result.truncated.tolist() == [True]
    assert result.reason["timeout"].tolist() == [True]


def test_phantomx_low_clearance_does_not_terminate_without_base_contact_force() -> None:
    task = _task()
    low = _state(task, ground_clearance=torch.tensor([0.01]))
    result = task.compute_terminations(_context(task, low, 1))

    assert result.terminated.tolist() == [False]
    assert result.reason["base_contact"].tolist() == [False]


def test_phantomx_reward_prefers_tracking_and_penalizes_action_changes() -> None:
    task = _task()
    state = _state(
        task,
        pose=torch.tensor([[0.51, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0]]),
        linear_velocity=torch.tensor([[0.5, 0.0, 0.0]]),
        angular_velocity=torch.tensor([[0.0, 0.0, 0.5]]),
        heading_command=torch.ones(1, 1),
    )
    context = _context(task, state)
    terminations = TerminationResult(
        terminated=torch.zeros(1, dtype=torch.bool),
        truncated=torch.zeros(1, dtype=torch.bool),
        reason={
            "base_contact": torch.zeros(1, dtype=torch.bool),
        },
    )

    tracking = task.compute_rewards(context, terminations)
    changed_context = replace(context, policy_actions=torch.ones(1, 18))
    changed = task.compute_rewards(changed_context, terminations)

    assert tracking.item() > 0.0
    assert changed.item() == pytest.approx(
        tracking.item() - 18 * task.phantomx_config.action_rate_weight
    )


def _command_task(command: PhantomXCommandConfig, batch_size: int = 1) -> PhantomXTask:
    return PhantomXTask(
        replace(PhantomXTaskConfig(), command=command),
        robot_spec=_robot_spec(),
        observation_shapes=SHAPES,
        batch_size=batch_size,
    )


def test_standing_command_is_zero_after_reset() -> None:
    command = replace(
        PhantomXCommandConfig(),
        standing_probability=1.0,
        resampling_time_min_s=5.0,
        resampling_time_max_s=5.0,
    )
    task = _command_task(command, batch_size=4)
    state = _state(task, num_envs=4)

    task.on_reset(torch.ones(4, dtype=torch.bool), state)
    velocity = task.published_velocity()

    assert velocity is not None
    assert torch.equal(velocity, torch.zeros(4, 3))


def test_command_holds_until_the_resample_interval_then_changes() -> None:
    command = replace(
        PhantomXCommandConfig(),
        standing_probability=0.0,
        initial_speed_min=0.2,
        initial_speed_max=0.8,
        resampling_time_min_s=0.05,
        resampling_time_max_s=0.05,
    )
    task = _command_task(command, batch_size=8)
    state = _state(task, num_envs=8)
    task.on_reset(torch.ones(8, dtype=torch.bool), state)
    held = task.published_velocity()
    assert held is not None

    task.refresh_command(state, dt=0.0)
    still_held = task.published_velocity()
    assert still_held is not None
    assert torch.equal(still_held, held)

    task.refresh_command(state, dt=1.0)
    changed = task.published_velocity()
    assert changed is not None
    assert torch.all(changed[:, 1] == 0.0)
    assert torch.all(changed[:, 2] == 0.0)
    assert not torch.equal(changed[:, 0], held[:, 0])


def test_enabled_turn_tracks_heading_without_waiting_for_distance() -> None:
    command = replace(
        PhantomXCommandConfig(),
        standing_probability=0.0,
        initial_speed_min=0.4,
        initial_speed_max=0.4,
        heading_delta_min=1.0,
        heading_delta_max=1.0,
        resampling_time_min_s=10.0,
        resampling_time_max_s=10.0,
    )
    task = _command_task(command)
    source = task.command_source
    assert isinstance(source, PhantomXVelocityCommandSource)
    source.bind_curriculum_term(
        type(
            "TurnOn",
            (),
            {
                "turn_enabled": torch.ones(1, dtype=torch.bool),
                "command_config": command,
                "run_seed": 0,
            },
        )()
    )
    state = _state(task)
    task.on_reset(torch.ones(1, dtype=torch.bool), state)
    started = task.published_velocity()
    assert started is not None
    assert torch.allclose(started[0], torch.tensor([0.4, 0.0, 0.5]), atol=1e-5)

    half = torch.sin(torch.tensor(0.5))
    whole = torch.cos(torch.tensor(0.5))
    reached = _state(
        task,
        pose=torch.tensor([[0.0, 0.0, 0.18, 0.0, 0.0, half, whole]]),
    )
    task.refresh_command(reached, dt=0.0)
    tracked = task.published_velocity()
    assert tracked is not None
    assert torch.allclose(tracked[0, 2], torch.tensor(0.0), atol=1e-5)


def test_training_noise_spares_the_command_and_keeps_the_critic_clean() -> None:
    task = _task()
    state = _state(task, num_envs=2, linear_velocity=torch.tensor([[0.3, 0.0, 0.0], [0.3, 0.0, 0.0]]))
    clean = task.build_observations(state, torch.ones(2, dtype=torch.bool), torch.zeros(2, 18))
    generator = torch.Generator()
    generator.manual_seed(1)
    task.enable_observation_corruption(generator)

    noisy = task.build_observations(state, torch.ones(2, dtype=torch.bool), torch.zeros(2, 18))

    assert not torch.equal(noisy["policy"][:, :3], clean["policy"][:, :3])
    assert torch.equal(noisy["policy"][:, 9:12], clean["policy"][:, 9:12])
    assert torch.equal(noisy["critic"][:, :109], clean["policy"])
    assert torch.equal(noisy["critic"][:, 109:], clean["critic"][:, 109:])


def test_clean_phantomx_actor_matches_export_plan_and_noise_keeps_export_unknown() -> None:
    assert PhantomXTask().capabilities.export.status is CapabilityStatus.UNKNOWN
    task = _task()
    state = _state(task, num_envs=2, linear_velocity=torch.tensor([[0.3, 0.0, 0.0], [0.3, 0.0, 0.0]]))
    previous_action = torch.zeros(2, 18)
    dt = torch.full((2,), 0.005)
    clean = task.build_observations(state, torch.ones(2, dtype=torch.bool), previous_action, dt)
    planned = PlanExecutor(task.observation_plan, command_channels=task.command_source.channels()).execute(
        PlanInputs(
            raw_state=state,
            commands=task.command_source.current(),
            previous_action=previous_action,
            control_frame_dt=dt.reshape(-1, 1),
        )
    )
    torch.testing.assert_close(clean["policy"], planned["policy"], atol=0, rtol=0)
    clean_report = task.capabilities
    assert clean_report.export.status is CapabilityStatus.SUPPORTED
    task.enable_observation_corruption(torch.Generator().manual_seed(1))
    noisy_report = task.capabilities
    assert noisy_report.export.status is CapabilityStatus.UNKNOWN


@pytest.mark.parametrize("reverse_actuators", [False, True])
def test_relative_joint_observations_use_configured_defaults_by_joint_name(reverse_actuators: bool) -> None:
    """Configured reference poses must drive both actor inputs and zero-action targets."""
    from math import prod

    reference = _robot_spec()
    expected_defaults = [0.01 * (index + 1) for index in range(18)]
    asset = replace(PHANTOMX_CFG, init_state=replace(
        PHANTOMX_CFG.init_state,
        joint_pos=dict(zip(PHANTOMX_JOINTS, expected_defaults, strict=True)),
    ))
    config = asset.to_robot_config()
    if reverse_actuators:
        config = replace(config, actuators=tuple(reversed(config.actuators)))
    spec = merge_robot_spec(config, reference.topology)
    task = PhantomXTask(PhantomXTaskConfig(), robot_spec=spec, observation_shapes=SHAPES)
    state = _state(task)
    for index, joint in enumerate(PHANTOMX_JOINTS):
        state[f"robot.joint.{joint}.joint_position"] = torch.tensor([expected_defaults[index]])
    state["robot.joint.thigh_rf.joint_position"] = torch.tensor([0.06])
    actions = torch.zeros(1, 18)
    observations = task.build_observations(state, torch.ones(1, dtype=torch.bool), actions)
    joint_start = 12 + prod(TERRAIN_SHAPE) + 1 + 6
    expected_relative = torch.zeros(1, 18)
    expected_relative[0, 1] = 0.04  # measured 0.06 rad minus configured 0.02 rad
    assert torch.allclose(observations["policy"][:, joint_start:joint_start + 18], expected_relative, atol=1e-7)
    exported_plan = ObservationPlan.from_json(task.observation_plan.to_json())
    exported = PlanExecutor(exported_plan, command_channels=task.command_source.channels()).execute(
        PlanInputs(
            raw_state=state, commands=task.command_source.current(), previous_action=actions,
            control_frame_dt=torch.full((1, 1), 0.005),
        )
    )
    assert torch.allclose(exported["policy"][:, joint_start:joint_start + 18], expected_relative, atol=1e-7)
    relative_op = next(op for op in task.observation_plan.ops if op.output == "joint_pos_rel")
    assert relative_op.params["default"] == pytest.approx(expected_defaults)
    target_defaults = list(reversed(expected_defaults)) if reverse_actuators else expected_defaults
    targets = task.preprocess_actions(actions, state)
    assert torch.allclose(targets[ROBOT_ACTUATOR_TARGET_FIELD], torch.tensor([target_defaults]), atol=1e-7)


def test_passive_joint_observation_reference_uses_reflected_default() -> None:
    from uerl.tasks.phantomx.composed import build_phantomx_observation_cfg

    reference = _robot_spec()
    config = PHANTOMX_CFG.to_robot_config()
    config = replace(config, actuators=tuple(act for act in config.actuators if act.joint != "thigh_rf"))
    topology = replace(reference.topology, joints=tuple(
        replace(joint, default_position=0.25) if joint.name == "thigh_rf" else joint
        for joint in reference.topology.joints
    ))
    spec = merge_robot_spec(config, topology)
    cfg = build_phantomx_observation_cfg(spec)
    defaults = cfg.groups["policy"].terms["joint_pos_rel"].params["default"]
    assert defaults == pytest.approx([0.0, 0.25, -0.30] + [0.0, 0.15, -0.30] * 5)


def test_direct_yaw_commands_cover_turn_in_place_without_curriculum() -> None:
    command = replace(
        PhantomXCommandConfig(), heading_command=False, standing_probability=0.0,
        turn_in_place_probability=1.0, yaw_rate_min=-0.5, yaw_rate_max=-0.5,
    )
    task = _command_task(command, batch_size=4)
    state = _state(task, num_envs=4)
    task.on_reset(torch.ones(4, dtype=torch.bool), state)
    actual = task.published_velocity()
    assert actual is not None
    assert torch.equal(actual, torch.tensor([[0.0, 0.0, -0.5]] * 4))


def test_direct_velocity_sampling_preserves_unreset_rows_and_completed_timer() -> None:
    command = replace(
        PhantomXCommandConfig(), heading_command=False, standing_probability=0.0,
        turn_in_place_probability=0.0, yaw_rate_min=-0.5, yaw_rate_max=0.5,
        resampling_time_min_s=0.04, resampling_time_max_s=0.04,
    )
    task = _command_task(command, batch_size=4)
    state = _state(task, num_envs=4)
    task.on_reset(torch.ones(4, dtype=torch.bool), state)
    before_value = task.published_velocity()
    assert before_value is not None
    before = before_value.clone()
    assert bool((before[:, 2] != 0).all())
    task.refresh_command(state, dt=0.02)
    held = task.published_velocity()
    assert held is not None
    assert torch.equal(held, before)
    task.on_reset(torch.tensor([True, False, True, False]), state)
    after_value = task.published_velocity()
    assert after_value is not None
    after = after_value.clone()
    assert torch.equal(after[[1, 3]], before[[1, 3]])
    assert not torch.equal(after[[0, 2]], before[[0, 2]])
    task.refresh_command(state, dt=0.02)
    final = task.published_velocity()
    assert final is not None
    assert torch.equal(final[[0, 2]], after[[0, 2]])
    assert not torch.equal(final[[1, 3]], after[[1, 3]])


def test_direct_command_mixture_contains_standing_turning_and_forward_rows() -> None:
    command = replace(PhantomXCommandConfig(), heading_command=False,
                      standing_probability=0.3, turn_in_place_probability=0.2,
                      initial_speed_min=0.05, initial_speed_max=0.5)
    task = _command_task(command, batch_size=128)
    task.on_reset(torch.ones(128, dtype=torch.bool), _state(task, num_envs=128))
    velocity = task.published_velocity()
    assert velocity is not None
    standing = (velocity == 0).all(dim=1)
    turning = (velocity[:, 0] == 0) & (velocity[:, 2] != 0)
    moving = velocity[:, 0] > 0
    assert bool(standing.any()) and bool(turning.any()) and bool(moving.any())
    assert bool((velocity[:, 1] == 0).all())
    assert bool((velocity[:, 2].abs() <= 0.5).all())
    assert bool((velocity[turning, 2] > 0).any()) and bool((velocity[turning, 2] < 0).any())
    assert bool((velocity[moving, 0] >= 0.05).all()) and bool((velocity[moving, 0] <= 0.5).all())


def test_uniform_velocity_commands_move_sideways_and_turn_from_first_reset() -> None:
    from uerl.tasks.phantomx.config import PhantomXCommandSampling

    command = replace(
        PhantomXCommandConfig(), sampling=PhantomXCommandSampling.UNIFORM_VELOCITY,
        initial_speed_min=-0.4, initial_speed_max=-0.4,
        lateral_speed_min=0.3, lateral_speed_max=0.3,
        heading_delta_min=1.0, heading_delta_max=1.0,
        standing_probability=0.0,
    )
    task = _command_task(command, batch_size=2)
    state = _state(task, num_envs=2)
    task.on_reset(torch.ones(2, dtype=torch.bool), state)
    actual = task.published_velocity()
    assert actual is not None
    # No curriculum promotion is needed: heading error 1 rad gives 0.5 rad/s.
    torch.testing.assert_close(actual, torch.tensor([[-0.4, 0.3, 0.5]] * 2))


def test_uniform_heading_is_absolute_and_uses_wrapped_error() -> None:
    from uerl.tasks.phantomx.config import PhantomXCommandSampling

    command = replace(PhantomXCommandConfig(),
                      sampling=PhantomXCommandSampling.UNIFORM_VELOCITY,
                      standing_probability=0.0, heading_delta_min=3.0, heading_delta_max=3.0)
    task = _command_task(command, batch_size=2)
    pose = torch.tensor([[0., 0., .18, 0., 0., math.sin(y / 2), math.cos(y / 2)]
                         for y in (-3.0, 0.0)])
    task.on_reset(torch.ones(2, dtype=torch.bool), _state(task, num_envs=2, pose=pose))
    actual = task.published_velocity()
    assert actual is not None
    # 6 rad wraps to 6 - 2*pi; the second row is clamped by the task's 1 rad/s cap.
    torch.testing.assert_close(actual[:, 2], torch.tensor([3.0 - math.pi, 1.0]))


def test_uniform_velocity_standing_zeros_every_component() -> None:
    from uerl.tasks.phantomx.config import PhantomXCommandSampling

    command = replace(PhantomXCommandConfig(),
                      sampling=PhantomXCommandSampling.UNIFORM_VELOCITY,
                      standing_probability=1.0, lateral_speed_min=-1.0, lateral_speed_max=1.0)
    task = _command_task(command, batch_size=4)
    task.on_reset(torch.ones(4, dtype=torch.bool), _state(task, num_envs=4))
    actual = task.published_velocity()
    assert actual is not None
    assert torch.equal(actual, torch.zeros(4, 3))


def test_uniform_velocity_sampling_distribution_and_sparse_timers() -> None:
    from uerl.tasks.phantomx.config import PhantomXCommandSampling

    command = replace(PhantomXCommandConfig(),
                      sampling=PhantomXCommandSampling.UNIFORM_VELOCITY,
                      standing_probability=0.02, initial_speed_min=-1.0, initial_speed_max=1.0,
                      lateral_speed_min=-1.0, lateral_speed_max=1.0,
                      resampling_time_min_s=10.0, resampling_time_max_s=10.0)
    task = _command_task(command, batch_size=1024)
    state = _state(task, num_envs=1024)
    task.on_reset(torch.ones(1024, dtype=torch.bool), state)
    value = task.published_velocity()
    assert value is not None
    before = value.clone()
    standing = (before == 0).all(dim=1)
    assert 0.005 < float(standing.float().mean()) < 0.04
    moving = before[~standing, :2]
    assert bool((moving.abs() <= 1).all())
    for axis in (0, 1):
        assert bool((moving[:, axis] < -0.8).any())
        assert bool((moving[:, axis] > 0.8).any())
    for x_positive in (False, True):
        for y_positive in (False, True):
            quadrant = ((moving[:, 0] > 0) == x_positive) & ((moving[:, 1] > 0) == y_positive)
            assert int(quadrant.sum()) > 100
    task.refresh_command(state, dt=9.0)
    held = task.published_velocity()
    assert held is not None
    assert torch.equal(held, before)
    mask = torch.zeros(1024, dtype=torch.bool)
    mask[0] = True
    task.on_reset(mask, state)
    reset = task.published_velocity()
    assert reset is not None
    after = reset.clone()
    assert torch.equal(after[1:], before[1:])
    task.refresh_command(state, dt=1.0)
    final = task.published_velocity()
    assert final is not None
    assert torch.equal(final[0], after[0])
    assert not torch.equal(final[1:], after[1:])

"""PhantomX DirectTask variant derivation and migration parity (ticket 28)."""

from __future__ import annotations

import json
from collections.abc import Mapping
from dataclasses import replace
from pathlib import Path
from typing import cast

import pytest
import torch

from tests.python.unit.robot_shape_fixtures import generic_robot_observation_shapes
from uerl.core.config.robot import ObsType, RobotSpec, RobotTopology, merge_robot_spec
from uerl.core.direct.robot_observation import robot_observation_schema
from uerl.core.direct.types import StepContext
from uerl.tasks.phantomx.commands import PhantomXVelocityCommandSource
from uerl.tasks.phantomx.config import (
    PHANTOMX_COMMAND_FIELDS,
    PHANTOMX_JOINTS,
    PhantomXTaskConfig,
    load_phantomx_training_config,
)
from uerl.tasks.phantomx.mdp_terms import PHANTOMX_METRIC_OWNERSHIP, collect_phantomx_metrics
from uerl.tasks.phantomx.task import (
    PhantomXTask,
    create_phantomx_direct_task,
    create_phantomx_pursuit_direct_task,
)

ROOT = Path(__file__).resolve().parents[3]
TRAINING_PATH = ROOT / "src" / "uerl" / "configs" / "tasks" / "phantomx" / "training.yaml"
PARITY_DIR = ROOT / "tests" / "parity" / "cases" / "phantomx_variants"
CONTROL_DT = 0.005 * 4
SHAPES = generic_robot_observation_shapes()
TERRAIN_SHAPE = SHAPES.shape_for(ObsType.TERRAIN_HEIGHT)

_VARIANT_CASES = (
    "walk_flat",
    "pursuit_flat",
    "continuous_terrain",
    "discrete_terrain",
)


def _robot_spec() -> RobotSpec:
    training = load_phantomx_training_config(TRAINING_PATH)
    body_names = ["base_link", *PHANTOMX_JOINTS]
    joints: list[dict[str, object]] = []
    for index, joint in enumerate(PHANTOMX_JOINTS):
        joints.append(
            {
                "name": joint,
                "parent_body_index": 0 if index % 3 == 0 else index,
                "child_body_index": index + 1,
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
                    "position_metres": [float(index - index % 3), 0.0, 0.0],
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


class _LatchSource:
    def __init__(self, values: Mapping[str, torch.Tensor]) -> None:
        self._values = values

    def current(self) -> Mapping[str, torch.Tensor]:
        return {name: self._values[name] for name in PHANTOMX_COMMAND_FIELDS}

    def channels(self) -> Mapping[str, int]:
        return {
            PHANTOMX_COMMAND_FIELDS[0]: 2,
            PHANTOMX_COMMAND_FIELDS[1]: 1,
            PHANTOMX_COMMAND_FIELDS[2]: 2,
        }


def _state(spec: RobotSpec, *, num_envs: int = 1) -> dict[str, torch.Tensor]:
    from uerl.tasks.phantomx.config import PHANTOMX_FEET

    joint_position = torch.tensor([a.default_pos for a in spec.actuators]).repeat(num_envs, 1)
    pose = torch.tensor([[0.4, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0]]).repeat(num_envs, 1)
    state: dict[str, torch.Tensor] = {}
    for item in robot_observation_schema(spec, SHAPES):
        name = cast(str, item["name"])
        semantic = cast(str, item["semantic"])
        if semantic == "joint_position":
            continue
        if semantic == "joint_velocity":
            continue
        if semantic == "body_pose":
            state[name] = pose
        elif semantic == "body_linear_velocity":
            state[name] = torch.tensor([[0.35, 0.0, 0.0]]).repeat(num_envs, 1)
        elif semantic == "body_angular_velocity":
            state[name] = torch.zeros(num_envs, 3)
        elif semantic == "ground_clearance":
            state[name] = torch.full((num_envs, 1), 0.18)
        elif semantic == "terrain_height":
            state[name] = torch.zeros(num_envs, *TERRAIN_SHAPE)
        elif semantic == "contact":
            state[name] = torch.zeros(num_envs, 1)
        elif semantic == "contact_force":
            state[name] = torch.zeros(num_envs, 1)
    for index, actuator in enumerate(spec.actuators):
        state[f"robot.joint.{actuator.joint}.joint_position"] = joint_position[:, index : index + 1]
        state[f"robot.joint.{actuator.joint}.joint_velocity"] = torch.zeros(num_envs, 1)
    del PHANTOMX_FEET
    state[PHANTOMX_COMMAND_FIELDS[0]] = torch.tensor([[0.4, 0.0]]).repeat(num_envs, 1)
    state[PHANTOMX_COMMAND_FIELDS[1]] = torch.zeros(num_envs, 1)
    state[PHANTOMX_COMMAND_FIELDS[2]] = torch.tensor([[0.5, 0.0]]).repeat(num_envs, 1)
    return state


def _context(task: PhantomXTask, state: dict[str, torch.Tensor]) -> StepContext:
    actions = torch.zeros(1, 18)
    physical = task.preprocess_actions(actions, state)
    return StepContext(
        raw_state=state,
        previous_policy_actions=actions,
        policy_actions=actions,
        physical_command=physical,
        transition_state=state,
        state_valid=torch.ones(1, dtype=torch.bool),
        slot_fault_code=torch.zeros(1, dtype=torch.uint16),
        episode_steps=torch.tensor([10]),
        transition_dt=CONTROL_DT,
        episode_elapsed_s=torch.tensor([10 * CONTROL_DT]),
    )


def _load_variant_case(stem: str) -> dict[str, object]:
    path = PARITY_DIR / f"{stem}.expected.json"
    return cast(dict[str, object], json.loads(path.read_text(encoding="utf-8")))


def _state_from_case(case: Mapping[str, object]) -> dict[str, torch.Tensor]:
    raw = cast(Mapping[str, list[float]], case["raw_state"])
    return {name: torch.tensor([values], dtype=torch.float32) for name, values in raw.items()}


def _direct_for_variant(stem: str, *, params: PhantomXTaskConfig, spec: RobotSpec) -> PhantomXTask:
    if stem == "pursuit_flat":
        return create_phantomx_pursuit_direct_task(
            params,
            robot_spec=spec,
            observation_shapes=SHAPES,
            control_dt=CONTROL_DT,
            batch_size=1,
            device="cpu",
        )
    return create_phantomx_direct_task(
        params,
        robot_spec=spec,
        observation_shapes=SHAPES,
        control_dt=CONTROL_DT,
        batch_size=1,
        device="cpu",
    )


def _is_pursuit_case(stem: str) -> bool:
    return stem == "pursuit_flat"


def test_ac_py_unit_phantomx_002_pursuit_differs_only_by_command_source() -> None:
    """AC_PY_UNIT_PHANTOMX_002: pursuit equals walk except command source."""

    from uerl.tasks.phantomx.commands import PhantomXPursuitCommandSource

    params = PhantomXTaskConfig()
    spec = _robot_spec()
    walk = create_phantomx_direct_task(
        params,
        robot_spec=spec,
        observation_shapes=SHAPES,
        control_dt=CONTROL_DT,
        batch_size=1,
        device="cpu",
    )
    pursuit = create_phantomx_pursuit_direct_task(
        params,
        robot_spec=spec,
        observation_shapes=SHAPES,
        control_dt=CONTROL_DT,
        batch_size=1,
        device="cpu",
    )
    assert type(walk) is type(pursuit) is PhantomXTask
    assert walk._cfg == pursuit._cfg
    assert isinstance(walk.command_source, PhantomXVelocityCommandSource)
    assert isinstance(pursuit.command_source, PhantomXPursuitCommandSource)


def test_ac_py_unit_phantomx_002_terrain_variants_use_replace() -> None:
    """AC_PY_UNIT_PHANTOMX_002: continuous/discrete are walk via replace()."""

    from uerl.tasks.phantomx.composed import (
        build_phantomx_continuous_terrain_cfg,
        build_phantomx_discrete_terrain_cfg,
        build_phantomx_walk_cfg,
    )
    from uerl.tasks.phantomx.config import (
        PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH,
        PHANTOMX_TERRAIN_CONFIG_PATH,
    )

    params = PhantomXTaskConfig()
    spec = _robot_spec()
    walk = build_phantomx_walk_cfg(params, spec, control_dt=CONTROL_DT)
    continuous = build_phantomx_continuous_terrain_cfg(params, spec, control_dt=CONTROL_DT)
    discrete = build_phantomx_discrete_terrain_cfg(params, spec, control_dt=CONTROL_DT)

    assert continuous.actions == walk.actions
    assert continuous.observations == walk.observations
    assert continuous.terminations == walk.terminations
    assert continuous.rewards == walk.rewards
    assert continuous.task == walk.task
    assert walk.terrain_config_path is None
    assert walk.curriculum is not None and set(walk.curriculum.terms) == {"command"}
    assert continuous.terrain_config_path == PHANTOMX_TERRAIN_CONFIG_PATH
    assert continuous.curriculum is not None
    assert set(continuous.curriculum.terms) == {"command", "terrain"}
    assert discrete == replace(
        continuous, terrain_config_path=PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH
    )


@pytest.mark.parametrize("case_stem", _VARIANT_CASES)
def test_ac_py_unit_phantomx_001_obs_reward_termination_parity(case_stem: str) -> None:
    """AC_PY_UNIT_PHANTOMX_001: each variant matches frozen corpus (tol 1e-6)."""

    from uerl.core.direct.robot_action import ROBOT_ACTUATOR_TARGET_FIELD

    case = _load_variant_case(case_stem)
    tol = float(cast(float, case["tolerance"]))
    expected = cast(Mapping[str, object], case["expected"])
    params = PhantomXTaskConfig()
    spec = _robot_spec()
    composed = _direct_for_variant(case_stem, params=params, spec=spec)
    state = _state_from_case(case)
    is_pursuit = _is_pursuit_case(case_stem)
    if not is_pursuit:
        composed.bind_curriculum_commands(_LatchSource(state))

    prev = torch.tensor(
        [cast(list[float], case["previous_policy_actions"])], dtype=torch.float32
    )
    valid = torch.ones(1, dtype=torch.bool)
    obs_composed = composed.build_observations(state, valid, prev)
    expected_policy = torch.tensor(cast(list[float], expected["policy"]), dtype=torch.float32)
    assert torch.allclose(obs_composed["policy"].reshape(-1), expected_policy, atol=tol, rtol=0.0)

    physical = composed.preprocess_actions(prev, state)
    expected_action = torch.tensor(cast(list[float], expected["action"]), dtype=torch.float32)
    assert torch.allclose(
        physical[ROBOT_ACTUATOR_TARGET_FIELD].reshape(-1),
        expected_action,
        atol=tol,
        rtol=0.0,
    )

    ctx_composed = composed.context_with_command(_context(composed, state))
    term_composed = composed.compute_terminations(ctx_composed)
    reward_manager = composed._require_rewards()
    term_vals = reward_manager.term_step_values(ctx_composed, term_composed)
    rew_composed = composed.compute_rewards(ctx_composed, term_composed)

    # Walk/terrain: compare the DirectTask path to a second fresh instance.
    if not is_pursuit:
        legacy = PhantomXTask(
            params,
            control_dt=CONTROL_DT,
            robot_spec=spec,
            observation_shapes=SHAPES,
        )
        obs_legacy = legacy.build_observations(state, valid, prev)
        assert torch.allclose(obs_legacy["policy"], obs_composed["policy"], atol=tol, rtol=0.0)
        phys_legacy = legacy.preprocess_actions(prev, state)
        assert torch.allclose(
            phys_legacy[ROBOT_ACTUATOR_TARGET_FIELD],
            physical[ROBOT_ACTUATOR_TARGET_FIELD],
            atol=tol,
            rtol=0.0,
        )
        ctx_legacy = legacy.context_with_command(_context(legacy, state))
        term_legacy = legacy.compute_terminations(ctx_legacy)
        assert torch.equal(term_legacy.terminated, term_composed.terminated)
        assert torch.equal(term_legacy.truncated, term_composed.truncated)
        rew_legacy = legacy.compute_rewards(ctx_legacy, term_legacy)
        assert torch.allclose(rew_legacy, rew_composed, atol=tol, rtol=0.0)

    assert term_composed.terminated.reshape(-1).tolist() == cast(list[float], expected["terminated"])
    assert term_composed.truncated.reshape(-1).tolist() == cast(list[float], expected["truncated"])
    expected_reward = torch.tensor(cast(list[float], expected["reward"]), dtype=torch.float32)
    assert torch.allclose(rew_composed.reshape(-1), expected_reward, atol=tol, rtol=0.0)

    expected_terms = cast(Mapping[str, list[float]], expected["reward_terms"])
    assert set(term_vals) == set(expected_terms)
    assert len(expected_terms) == 11
    for name, values in expected_terms.items():
        assert torch.allclose(
            term_vals[name].reshape(-1),
            torch.tensor(values, dtype=torch.float32),
            atol=tol,
            rtol=0.0,
        )

    assert set(cast(list[str], expected["metric_keys"])) == set(PHANTOMX_METRIC_OWNERSHIP)


def test_ac_py_unit_phantomx_001_corpus_covers_four_variants() -> None:
    """AC_PY_UNIT_PHANTOMX_001: corpus directory has one case per variant."""

    present = {
        path.name.removesuffix(".expected.json")
        for path in PARITY_DIR.glob("*.expected.json")
    }
    assert present == set(_VARIANT_CASES)


def test_ac_py_unit_phantomx_003_metric_keys_match_task() -> None:
    """AC_PY_UNIT_PHANTOMX_003: phantomx/* metric key sets are identical."""

    params = PhantomXTaskConfig()
    spec = _robot_spec()
    task = PhantomXTask(params, robot_spec=spec, observation_shapes=SHAPES)
    state = _state(spec)
    ctx = task.context_with_command(_context(task, state))
    terms = task.compute_terminations(ctx)
    task_keys = {
        key for key in task.collect_metrics(ctx, terms) if key.startswith("phantomx/")
    }
    composed_keys = set(collect_phantomx_metrics(ctx, terms, config=params, robot_spec=spec))
    assert composed_keys == task_keys
    assert set(PHANTOMX_METRIC_OWNERSHIP) == composed_keys
    assert set(PHANTOMX_METRIC_OWNERSHIP.values()) <= {"lib", "reward_manager", "task"}
    # Ticket 29: Episode_Termination/* is named by DirectTask, not the env.
    assert {"Episode_Termination/base_contact", "Episode_Termination/timeout"} <= set(
        task.collect_metrics(ctx, terms)
    )


def test_registry_factories_return_direct_tasks() -> None:
    from uerl.tasks.phantomx.config import (
        PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH,
        PHANTOMX_TERRAIN_CONFIG_PATH,
    )
    from uerl.tasks.phantomx.registration import (
        create_phantomx_continuous_terrain_task,
        create_phantomx_discrete_terrain_task,
        create_phantomx_pursuit_task,
        create_phantomx_task,
    )

    walk = create_phantomx_task(
        PhantomXTaskConfig(), robot_spec=_robot_spec(), observation_shapes=SHAPES
    )
    pursuit = create_phantomx_pursuit_task(
        PhantomXTaskConfig(), robot_spec=_robot_spec(), observation_shapes=SHAPES
    )
    continuous = create_phantomx_continuous_terrain_task(
        PhantomXTaskConfig(), robot_spec=_robot_spec(), observation_shapes=SHAPES
    )
    discrete = create_phantomx_discrete_terrain_task(
        PhantomXTaskConfig(), robot_spec=_robot_spec(), observation_shapes=SHAPES
    )
    assert isinstance(walk, PhantomXTask)
    assert isinstance(pursuit, PhantomXTask)
    assert isinstance(continuous, PhantomXTask)
    assert isinstance(discrete, PhantomXTask)
    assert isinstance(walk._velocity_source, PhantomXVelocityCommandSource)
    assert type(walk) is type(pursuit) is type(continuous) is type(discrete)
    assert walk._cfg is not None and walk._cfg.terrain_config_path is None
    assert continuous._cfg is not None
    assert continuous._cfg.terrain_config_path == PHANTOMX_TERRAIN_CONFIG_PATH
    assert discrete._cfg is not None
    assert discrete._cfg.terrain_config_path == PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH
    assert continuous._cfg.actions == walk._cfg.actions
    assert continuous._cfg.observations == walk._cfg.observations

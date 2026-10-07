"""Verify RewardManager weighting, episode sums, and reward terms."""

from __future__ import annotations

import math
from dataclasses import replace
from typing import Any, cast

import pytest
import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec, RobotTopology, merge_robot_spec, parse_robot_config
from uerl.core.direct.types import PhysicalCommandBatch, StepContext, TerminationResult
from uerl.core.mdp.lib import rewards as reward_lib
from uerl.core.mdp.managers import RewardManager
from uerl.core.mdp.terms import RewardCfg, RewTermCfg
from uerl.errors import ConfigError
from uerl.tasks.phantomx.config import load_phantomx_training_config

# Explicit episode-log key set (Isaac Lab RewardManager extras prefix).
_EXPECTED_EPISODE_KEYS = {
    "Episode_Reward/track",
    "Episode_Reward/penalty",
    "Episode_Reward/bonus",
}

# Reward weights apply once per control transition, independently of physical
# decimation. The table remains explicit so the migration oracle covers every
# PhantomX term.
_PHANTOMX_WEIGHT_CONVERSION: dict[str, tuple[float, float]] = {
    # name: (legacy_per_step_weight, current_per_step_weight)
    "linear_velocity_tracking": (1.5, 1.5),
    "linear_velocity_progress": (1.0, 1.0),
    "yaw_rate_tracking": (0.75, 0.75),
    "yaw_rate_progress": (0.75, 0.75),
    "vertical_velocity": (2.0, 2.0),
    "body_angular_velocity": (0.05, 0.05),
    "upright": (2.5, 2.5),
    "body_clearance": (200.0, 200.0),
    "joint_velocity": (0.005, 0.005),
    "fall": (1.0, 1.0),
}


def _context(
    *,
    rows: int = 2,
    transition_state: dict[str, torch.Tensor] | None = None,
    policy_actions: torch.Tensor | None = None,
    previous_policy_actions: torch.Tensor | None = None,
    transition_dt: float | None = None,
) -> StepContext:
    return StepContext(
        raw_state={},
        previous_policy_actions=previous_policy_actions
        if previous_policy_actions is not None
        else torch.zeros(rows, 2),
        policy_actions=policy_actions if policy_actions is not None else torch.zeros(rows, 2),
        physical_command=PhysicalCommandBatch({}),
        transition_state=transition_state or {},
        state_valid=torch.ones(rows, dtype=torch.bool),
        slot_fault_code=torch.zeros(rows, dtype=torch.uint16),
        episode_steps=torch.zeros(rows, dtype=torch.long),
        transition_dt=transition_dt,
    )


def _empty_terminations(rows: int = 2) -> TerminationResult:
    return TerminationResult(
        terminated=torch.zeros(rows, dtype=torch.bool),
        truncated=torch.zeros(rows, dtype=torch.bool),
        reason={},
    )


def _tiny_spec() -> RobotSpec:
    config = parse_robot_config(
        """
actuators:
  - joint: hip
    stiffness: 1.0
    damping: 0.1
    effort_limit: 1.0
    default_pos: 0.0
    action_scale: 1.0
observations:
  - type: joint_position
    joint: hip
  - type: body_pose
    body: base
reset:
  distributions:
    - type: joint_position
      joint: hip
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.hip
"""
    )
    frame = {
        "position_metres": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    topology = RobotTopology.from_response(
        {
            "asset_path": "/Game/Test/SK_Test",
            "body_names": ["base", "link"],
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
                }
            ],
        }
    )
    return merge_robot_spec(config, topology)


def _constant_term(value: float) -> object:
    def _term(ctx: StepContext, **_: object) -> torch.Tensor:
        return torch.full(ctx.episode_steps.shape, value, dtype=torch.float32)

    return _term


def test_ac_py_unit_rewmgr_001_weighted_sum_with_negative_weight() -> None:
    """AC_PY_UNIT_REWMGR_001: three terms combine; negative weight reduces total."""

    spec = _tiny_spec()
    manager = RewardManager(
        RewardCfg(
            terms={
                "track": RewTermCfg(func=_constant_term(1.0), weight=2.0),
                "penalty": RewTermCfg(func=_constant_term(1.0), weight=-0.5),
                "bonus": RewTermCfg(func=_constant_term(0.25), weight=4.0),
            }
        ),
        spec,
        batch_size=2,
        device="cpu",
    )
    # 2*1 + (-0.5)*1 + 4*0.25 = 2.5
    total = manager.compute(_context(), _empty_terminations())
    assert torch.allclose(total, torch.full((2,), 2.5))


def test_ac_py_unit_rewmgr_002_reward_is_independent_of_physics_dt() -> None:
    """AC_PY_UNIT_REWMGR_002: one transition has one reward regardless of physical dt."""

    spec = _tiny_spec()
    cfg = RewardCfg(terms={"track": RewTermCfg(func=_constant_term(1.0), weight=3.0)})
    a = RewardManager(cfg, spec, batch_size=1, device="cpu")
    b = RewardManager(cfg, spec, batch_size=1, device="cpu")
    ctx = _context(rows=1, transition_dt=0.005)
    term = _empty_terminations(1)
    reward_a = a.compute(ctx, term)
    reward_b = b.compute(replace(ctx, transition_dt=0.035), term)
    assert torch.equal(reward_a, torch.tensor([3.0]))
    assert torch.equal(reward_b, torch.tensor([3.0]))


def test_ac_py_unit_rewmgr_007_rate_reward_integrates_time_but_fall_cost_is_once() -> None:
    """A 35 ms transition earns seven times the 5 ms flow reward."""

    cfg = RewardCfg(
        reference_dt_s=0.02,
        terms={
            "tracking": RewTermCfg(func=_constant_term(1.0), weight=2.0, time_mode="rate"),
            "fall": RewTermCfg(func=_constant_term(1.0), weight=-3.0),
        },
    )
    spec = _tiny_spec()
    short = RewardManager(cfg, spec, batch_size=1, device="cpu")
    long = RewardManager(cfg, spec, batch_size=1, device="cpu")

    assert short.compute(_context(rows=1, transition_dt=0.005), _empty_terminations(1)).tolist() == [-2.5]
    assert long.compute(_context(rows=1, transition_dt=0.035), _empty_terminations(1)).tolist() == [0.5]
    assert short.episode_log(torch.tensor([True]))["Episode_Reward/fall"] == -3.0
    assert long.episode_log(torch.tensor([True]))["Episode_Reward/fall"] == -3.0


def test_ac_py_unit_rewmgr_008_rate_reward_requires_reference_interval_at_assembly() -> None:
    """A rate term without a reference interval fails before the rollout starts."""

    cfg = RewardCfg(terms={"tracking": RewTermCfg(func=_constant_term(1.0), weight=1.0, time_mode="rate")})
    with pytest.raises(ConfigError, match="reference_dt_s"):
        RewardManager(cfg, _tiny_spec(), batch_size=1, device="cpu")


def test_ac_py_unit_rewmgr_003_partial_reset_clears_selected_rows() -> None:
    """AC_PY_UNIT_REWMGR_003: episode sums accumulate; reset(mask) clears only selected rows."""

    spec = _tiny_spec()
    manager = RewardManager(
        RewardCfg(terms={"track": RewTermCfg(func=_constant_term(1.0), weight=1.0)}),
        spec,
        batch_size=3,
        device="cpu",
    )
    ctx = _context(rows=3)
    term = _empty_terminations(3)
    manager.compute(ctx, term)
    manager.compute(ctx, term)
    assert manager.episode_log(torch.tensor([True, True, True]))["Episode_Reward/track"] == 2.0

    mask = torch.tensor([True, False, True])
    manager.reset(mask)
    assert manager.episode_log(torch.tensor([True, False, True]))["Episode_Reward/track"] == 0.0
    assert manager.episode_log(torch.tensor([False, True, False]))["Episode_Reward/track"] == 2.0


def test_ac_py_unit_rewmgr_004_episode_log_keys_exact() -> None:
    """AC_PY_UNIT_REWMGR_004: episode log keys match the explicit expected set."""

    spec = _tiny_spec()
    manager = RewardManager(
        RewardCfg(
            terms={
                "track": RewTermCfg(func=_constant_term(1.0), weight=1.0),
                "penalty": RewTermCfg(func=_constant_term(1.0), weight=-1.0),
                "bonus": RewTermCfg(func=_constant_term(1.0), weight=0.5),
            }
        ),
        spec,
        batch_size=2,
        device="cpu",
    )
    manager.compute(_context(), _empty_terminations())
    log = manager.episode_log(torch.tensor([True, False]))
    assert set(log) == _EXPECTED_EPISODE_KEYS
    assert log["Episode_Reward/track"] == pytest.approx(1.0)
    assert log["Episode_Reward/penalty"] == pytest.approx(-1.0)
    assert log["Episode_Reward/bonus"] == pytest.approx(0.5)


def test_ac_py_unit_rewmgr_006_sparse_valid_rows_accumulate_on_stable_slot_ids() -> None:
    """A faulted middle Slot cannot receive another Slot's episode reward."""

    manager = RewardManager(
        RewardCfg(terms={"track": RewTermCfg(func=_constant_term(1.0), weight=1.0)}),
        _tiny_spec(),
        batch_size=3,
        device="cpu",
    )
    compact = replace(_context(rows=2), slot_ids=torch.tensor([0, 2]))
    manager.compute(compact, _empty_terminations(2))
    assert manager.episode_log(torch.tensor([False, True, False]))["Episode_Reward/track"] == 0.0
    assert manager.episode_log(torch.tensor([False, False, True]))["Episode_Reward/track"] == 1.0


def test_ac_py_unit_rewmgr_005_empty_and_duplicate_name_rejected() -> None:
    """AC_PY_UNIT_REWMGR_005: zero terms and duplicate names fail at assemble."""

    spec = _tiny_spec()
    with pytest.raises(ConfigError) as empty:
        RewardManager(RewardCfg(terms={}), spec, batch_size=1, device="cpu")
    assert empty.value.path == "terms"

    class _DupTerms(dict[str, RewTermCfg]):
        def items(self) -> Any:
            term = RewTermCfg(func=_constant_term(1.0), weight=1.0)
            yield "track", term
            yield "track", term

        def __bool__(self) -> bool:
            return True

    with pytest.raises(ConfigError, match="duplicate"):
        RewardManager(RewardCfg(terms=_DupTerms()), spec, batch_size=1, device="cpu")


def test_reward_manager_resolves_entity_params_at_assemble() -> None:
    """params RobotEntityCfg is resolve()'d during RewardManager construction."""

    spec = _tiny_spec()
    entity = RobotEntityCfg(joint_names="hip")
    assert not entity.resolved
    manager = RewardManager(
        RewardCfg(
            terms={
                "torque": RewTermCfg(
                    func=reward_lib.joint_torque_l2,
                    weight=1.0,
                    params={"entity": entity},
                )
            }
        ),
        spec,
        batch_size=1,
        device="cpu",
    )
    assert entity.resolved
    assert manager.term_names == ("torque",)


def test_phantomx_weight_conversion_equivalence() -> None:
    """PhantomX reward terms other than the recalibrated action rate keep their weighted sum."""

    training = load_phantomx_training_config()
    assert training.worker.decimation == (1, 7)
    for name, (_legacy_weight, current_weight) in _PHANTOMX_WEIGHT_CONVERSION.items():
        field_name = {
            "joint_velocity": "joint_velocity_penalty",
            "fall": "fall_penalty",
        }.get(name, f"{name}_weight")
        assert getattr(training.task, field_name) == pytest.approx(current_weight)

    # Synthetic term values stand in for one PhantomX reward step.
    term_values = {
        "linear_velocity_tracking": 0.8,
        "linear_velocity_progress": 0.4,
        "yaw_rate_tracking": 0.7,
        "yaw_rate_progress": 0.3,
        "vertical_velocity": 0.2,
        "body_angular_velocity": 0.1,
        "upright": 0.05,
        "body_clearance": 0.01,
        "joint_velocity": 0.25,
        "fall": 0.0,
    }
    legacy = 0.0
    for name, value in term_values.items():
        old_weight, _new_weight = _PHANTOMX_WEIGHT_CONVERSION[name]
        # Penalties in PhantomX are subtracted (positive configured weight).
        sign = (
            -1.0
            if name
            in {
                "vertical_velocity",
                "body_angular_velocity",
                "upright",
                "body_clearance",
                "joint_velocity",
                "fall",
            }
            else 1.0
        )
        legacy += sign * old_weight * value

    spec = _tiny_spec()
    terms: dict[str, RewTermCfg] = {}
    for name, value in term_values.items():
        _old, new_weight = _PHANTOMX_WEIGHT_CONVERSION[name]
        sign = (
            -1.0
            if name
            in {
                "vertical_velocity",
                "body_angular_velocity",
                "upright",
                "body_clearance",
                "joint_velocity",
                "fall",
            }
            else 1.0
        )
        terms[name] = RewTermCfg(func=_constant_term(value), weight=sign * new_weight)

    manager = RewardManager(RewardCfg(terms=terms), spec, batch_size=1, device="cpu")
    total = manager.compute(_context(rows=1), _empty_terminations(1))
    assert float(total.item()) == pytest.approx(legacy, abs=1e-6)


def test_reward_lib_track_lin_vel_xy() -> None:
    spec = _tiny_spec()
    entity = RobotEntityCfg(body_names="base")
    entity.resolve(spec)
    # Identity quat; body vel == world vel. Command matches → reward 1.
    ctx = _context(
        rows=1,
        transition_state={
            "robot.body.base.body_pose": torch.tensor([[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0]]),
            "robot.body.base.body_linear_velocity": torch.tensor([[0.5, 0.0, 0.0]]),
            "command.lin_vel": torch.tensor([[0.5, 0.0, 0.0]]),
        },
    )
    matched = reward_lib.track_lin_vel_xy(ctx, entity=entity, command_channel="command.lin_vel", std=0.25)
    assert float(matched.item()) == pytest.approx(1.0)

    ctx_miss = _context(
        rows=1,
        transition_state={
            "robot.body.base.body_pose": torch.tensor([[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0]]),
            "robot.body.base.body_linear_velocity": torch.tensor([[0.0, 0.0, 0.0]]),
            "command.lin_vel": torch.tensor([[0.5, 0.0, 0.0]]),
        },
    )
    missed = reward_lib.track_lin_vel_xy(ctx_miss, entity=entity, command_channel="command.lin_vel", std=0.25)
    expected = math.exp(-(0.5**2) / (0.25**2))
    assert float(missed.item()) == pytest.approx(expected)


def test_reward_lib_joint_torque_l2() -> None:
    spec = _tiny_spec()
    entity = RobotEntityCfg(joint_names="hip")
    entity.resolve(spec)
    ctx = _context(
        rows=1,
        transition_state={"robot.joint.hip.applied_torque": torch.tensor([[2.0, -1.0]])},
    )
    assert float(reward_lib.joint_torque_l2(ctx, entity=entity).item()) == pytest.approx(5.0)


def test_ac_py_unit_dtnorm_001_action_rate_is_the_squared_change_for_any_interval() -> None:
    """AC_PY_UNIT_DTNORM_001: the term is sum((a_k - a_(k-1))²), never divided by dt."""

    ctx = _context(
        rows=2,
        policy_actions=torch.tensor([[1.0, -1.0], [3.0, 4.0]]),
        previous_policy_actions=torch.tensor([[0.0, 0.0], [3.0, 4.0]]),
        transition_dt=0.005,
    )
    expected = torch.tensor([2.0, 0.0])

    torch.testing.assert_close(reward_lib.action_rate_l2(ctx), expected)
    torch.testing.assert_close(reward_lib.action_rate_l2(replace(ctx, transition_dt=0.035)), expected)


def test_ac_py_unit_dtnorm_002_phantomx_exploration_noise_is_a_small_action_rate_cost() -> None:
    """AC_PY_UNIT_DTNORM_002: init-std noise costs ~1.4% of the tracking ceiling per episode."""

    training = load_phantomx_training_config()
    sigma = float(cast(float, training.runner.parameters["init_std"]))
    reference_dt = cast(float, training.task.reference_dt_s)
    weight = training.task.action_rate_weight
    manager = RewardManager(
        RewardCfg(
            reference_dt_s=reference_dt,
            terms={"action_rate": RewTermCfg(func=reward_lib.action_rate_l2, weight=-weight, time_mode="rate")},
        ),
        _tiny_spec(),
        batch_size=20_000,
        device="cpu",
    )
    generator = torch.Generator().manual_seed(0)
    previous = sigma * torch.randn(20_000, 18, generator=generator)
    current = sigma * torch.randn(20_000, 18, generator=generator)
    ctx = _context(rows=20_000, policy_actions=current, previous_policy_actions=previous)

    reference = manager.term_step_values(replace(ctx, transition_dt=reference_dt), _empty_terminations(20_000))
    longest = manager.term_step_values(replace(ctx, transition_dt=0.035), _empty_terminations(20_000))
    per_frame = -float(reference["action_rate"].mean())
    per_episode = per_frame * cast(float, training.task.max_episode_duration_s) / reference_dt
    tracking_ceiling = training.task.linear_velocity_tracking_weight * 1000.0

    assert per_frame == pytest.approx(weight * 36.0 * sigma**2, rel=0.02)
    torch.testing.assert_close(longest["action_rate"], reference["action_rate"] * 1.75)
    assert per_episode == pytest.approx(21.6, rel=0.02)
    assert per_episode / tracking_ceiling < 0.02


def test_reward_lib_flat_orientation_l2() -> None:
    spec = _tiny_spec()
    entity = RobotEntityCfg(body_names="base")
    entity.resolve(spec)
    # Identity orientation → projected gravity (0, 0, -1) → xy L2 = 0.
    flat = reward_lib.flat_orientation_l2(
        _context(
            rows=1,
            transition_state={"robot.body.base.body_pose": torch.tensor([[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0]])},
        ),
        entity=entity,
    )
    assert float(flat.item()) == pytest.approx(0.0)


def test_reward_manager_distinct_rows_keep_weights_and_sparse_episode_sums() -> None:
    """Different compact rows retain their values at stable Slots 0 and 2."""

    manager = RewardManager(
        RewardCfg(
            terms={
                "track": RewTermCfg(func=lambda ctx: ctx.transition_state["track"], weight=2.0),
                "cost": RewTermCfg(func=lambda ctx: ctx.transition_state["cost"], weight=-0.5),
            }
        ),
        _tiny_spec(),
        batch_size=3,
        device="cpu",
    )
    full = _context(
        rows=3, transition_state={"track": torch.tensor([1.0, 3.0, -2.0]), "cost": torch.tensor([4.0, 1.0, 0.0])}
    )
    torch.testing.assert_close(manager.compute(full, _empty_terminations(3)), torch.tensor([0.0, 5.5, -4.0]))
    compact = replace(
        _context(rows=2, transition_state={"track": torch.tensor([2.0, -1.0]), "cost": torch.tensor([2.0, 6.0])}),
        slot_ids=torch.tensor([0, 2]),
    )
    torch.testing.assert_close(manager.compute(compact, _empty_terminations(2)), torch.tensor([3.0, -5.0]))
    # Slot 1 did not participate in the second step. Logs expose each Slot separately.
    for slot, track, cost in ((0, 6.0, -3.0), (1, 6.0, -0.5), (2, -6.0, -3.0)):
        log = manager.episode_log(torch.arange(3).eq(slot))
        assert log["Episode_Reward/track"] == track
        assert log["Episode_Reward/cost"] == cost
    manager.reset(torch.tensor([True, False, True]))
    assert manager.episode_log(torch.tensor([False, True, False])) == {
        "Episode_Reward/track": 6.0,
        "Episode_Reward/cost": -0.5,
    }
    assert manager.episode_log(torch.tensor([True, False, True])) == {
        "Episode_Reward/track": 0.0,
        "Episode_Reward/cost": 0.0,
    }

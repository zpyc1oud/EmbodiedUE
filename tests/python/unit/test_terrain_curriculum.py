"""Verify Isaac Lab-style terrain level progression without UE or Socket I/O."""

from __future__ import annotations

import math

import pytest
import torch

from uerl import CurriculumManager, CurriculumStep, TerrainCurriculum
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm

POSE_FIELD = "robot.body.base_link.body_pose"


def test_curriculum_promotes_holds_and_demotes_per_slot() -> None:
    """Use command progress and commanded path length independently per Slot."""

    curriculum = TerrainCurriculum(num_levels=3, num_envs=2, terrain_size_x=10.0)

    assert curriculum.levels == (0, 0)
    assert curriculum.update([0], [6.0], [10.0]) == (1, 0)
    assert curriculum.update([1], [5.0], [10.0]) == (1, 0)
    assert curriculum.update([1], [4.9], [10.0]) == (1, 0)
    assert curriculum.update([0, 1], [6.0, 4.0], [10.0, 10.0]) == (2, 0)


def test_curriculum_caps_promotion_at_reachable_command_distance() -> None:
    """Keep an enlarged UE atlas from making Isaac-style promotion unreachable."""

    curriculum = TerrainCurriculum(num_levels=2, num_envs=1, terrain_size_x=30.0)

    assert curriculum.update([0], [8.1], [10.0]) == (1,)


def test_curriculum_rejects_invalid_inputs_and_demotes_backward_progress() -> None:
    """Reject malformed inputs; progress against the command is a valid demotion."""

    curriculum = TerrainCurriculum(num_levels=2, num_envs=1, terrain_size_x=10.0)
    with pytest.raises(ValueError, match="equal lengths"):
        curriculum.update([0], [1.0], [])
    with pytest.raises(ValueError, match="finite"):
        curriculum.update([0], [float("nan")], [1.0])
    with pytest.raises(ValueError, match="invalid curriculum Slot"):
        curriculum.update([1], [1.0], [1.0])
    with pytest.raises(ValueError, match="non-negative"):
        curriculum.update([0], [1.0], [-1.0])

    assert curriculum.update([0], [6.0], [10.0]) == (1,)
    assert curriculum.update([0], [-1.0], [10.0]) == (0,)


def test_ac_py_unit_terrain_002_rollover_stays_in_range_and_demotion_stops_at_zero() -> None:
    """Never emit a terrain level outside the negotiated range."""

    curriculum = TerrainCurriculum(num_levels=2, num_envs=1, terrain_size_x=2.0)
    assert curriculum.update([0], [2.0], [10.0]) == (1,)
    assert curriculum.update([0], [2.0], [10.0]) == (1,)
    assert curriculum.update([0], [0.0], [10.0]) == (0,)
    assert curriculum.update([0], [0.0], [10.0]) == (0,)


def test_ac_py_unit_terrain_001_maximum_success_revisits_lower_terrain_and_restores_rng() -> None:
    """The hardest tier must not permanently absorb a successful Slot."""

    term = TerrainLevelTerm(num_levels=3, num_envs=1, terrain_size_x=10.0, seed=7)
    for _ in range(2):
        term.reset(torch.ones(1, dtype=torch.bool), _pose_at(0.0))
        term.update(_step(6.0))
    assert term.levels().tolist() == [2]

    checkpoint = term.state_dict()
    term.reset(torch.ones(1, dtype=torch.bool), _pose_at(0.0))
    term.update(_step(6.0))
    assert term.levels().tolist() == [1]

    restored = TerrainLevelTerm(num_levels=3, num_envs=1, terrain_size_x=10.0, seed=999)
    restored.load_state_dict(checkpoint)
    restored.reset(torch.ones(1, dtype=torch.bool), _pose_at(0.0))
    restored.update(_step(6.0))
    assert restored.levels().tolist() == [1]


def _pose(x: float, y: float = 0.0, yaw: float = 0.0, *, num_envs: int = 1) -> dict[str, torch.Tensor]:
    pose = torch.tensor(
        [[x, y, 0.18, 0.0, 0.0, math.sin(yaw / 2.0), math.cos(yaw / 2.0)]],
        dtype=torch.float32,
    )
    return {POSE_FIELD: pose.repeat(num_envs, 1)}


def _pose_at(x: float, *, num_envs: int = 1) -> dict[str, torch.Tensor]:
    return _pose(x, num_envs=num_envs)


def _transition(
    state: dict[str, torch.Tensor],
    command: list[list[float]],
    *,
    transition_dt: float,
    terminated: list[bool] | None = None,
    truncated: list[bool] | None = None,
) -> CurriculumStep:
    num_envs = len(command)
    return CurriculumStep(
        transition_state=state,
        metrics={},
        state_valid=torch.ones(num_envs, dtype=torch.bool),
        terminated=torch.tensor(terminated if terminated is not None else [False] * num_envs),
        truncated=torch.tensor(truncated if truncated is not None else [True] * num_envs),
        episode_lengths=torch.full((num_envs,), 10),
        transition_dt=transition_dt,
        command_velocity=torch.tensor(command, dtype=torch.float32),
    )


def _step(
    x: float,
    *,
    num_envs: int = 1,
    transition_dt: float = 10.0,
    speed: float = 1.0,
    terminated: bool = False,
) -> CurriculumStep:
    return _transition(
        _pose_at(x, num_envs=num_envs),
        [[speed, 0.0]] * num_envs,
        transition_dt=transition_dt,
        terminated=[terminated] * num_envs,
    )


def _walk_arc(
    term: TerrainLevelTerm,
    *,
    speed_scale: float,
    turn_angle: float,
    command_speed: float = 0.45,
    duration_s: float = 20.0,
    dt: float = 0.02,
) -> None:
    """Follow a forward command whose heading turns uniformly by ``turn_angle``."""

    steps = round(duration_s / dt)
    yaw_rate = turn_angle / duration_s
    x = y = yaw = 0.0
    term.reset(torch.ones(1, dtype=torch.bool), _pose(x, y, yaw))
    for index in range(steps):
        speed = command_speed * speed_scale
        x += speed * math.cos(yaw) * dt
        y += speed * math.sin(yaw) * dt
        yaw += yaw_rate * dt
        term.update(
            _transition(
                _pose(x, y, yaw),
                [[command_speed, 0.0]],
                transition_dt=dt,
                truncated=[index == steps - 1],
            )
        )


def test_terrain_level_term_promotes_perfect_tracking_on_a_turning_path() -> None:
    """A tracked U-turn walks its full commanded path although its chord is short."""

    straight = TerrainLevelTerm(num_levels=3, num_envs=1, terrain_size_x=30.0)
    turning = TerrainLevelTerm(num_levels=3, num_envs=1, terrain_size_x=30.0)
    _walk_arc(straight, speed_scale=1.0, turn_angle=0.0)
    _walk_arc(turning, speed_scale=1.0, turn_angle=math.pi)

    path = 0.45 * 20.0
    assert torch.allclose(turning.commanded_distances(), torch.tensor([path]))
    # The chord of a half circle is 2/pi of its arc, below the 0.8 promotion fraction.
    assert torch.allclose(turning.progress(), torch.tensor([path]), rtol=1e-3)
    assert straight.levels().tolist() == turning.levels().tolist() == [1]


def test_terrain_level_term_gives_overspeed_no_edge_and_demotes_standing_still() -> None:
    """Overspeed promotes exactly as perfect tracking; a Robot that stays put demotes."""

    levels: dict[float, int] = {}
    for speed_scale in (1.0, 1.4, 0.0):
        term = TerrainLevelTerm(num_levels=3, num_envs=1, terrain_size_x=30.0)
        term.load_state_dict({**term.state_dict(), "levels": [1]})
        _walk_arc(term, speed_scale=speed_scale, turn_angle=math.pi)
        levels[speed_scale] = int(term.levels().item())

    assert levels == {1.0: 2, 1.4: 2, 0.0: 0}


def test_terrain_level_term_projects_body_frame_command_by_root_heading() -> None:
    """A lateral body command faced along world +y turns +x displacement into backing up."""

    term = TerrainLevelTerm(num_levels=3, num_envs=2, terrain_size_x=30.0)
    term.load_state_dict({**term.state_dict(), "levels": [1, 1]})
    facing_y = math.pi / 2.0
    start = torch.cat((_pose(0.0, yaw=facing_y)[POSE_FIELD], _pose(0.0, yaw=facing_y)[POSE_FIELD]))
    term.reset(torch.ones(2, dtype=torch.bool), {POSE_FIELD: start})
    end = torch.cat((_pose(0.0, 9.0, facing_y)[POSE_FIELD], _pose(9.0, 0.0, facing_y)[POSE_FIELD]))
    # Body +x faces world +y; body +y faces world -x.
    term.update(_transition({POSE_FIELD: end}, [[1.0, 0.0], [0.0, 1.0]], transition_dt=10.0))

    assert torch.allclose(term.progress(), torch.tensor([9.0, -9.0]), atol=1e-5)
    assert term.levels().tolist() == [2, 0]


def test_terrain_level_term_ignores_motion_under_a_standing_command() -> None:
    """Drift while commanded to stand adds neither progress nor commanded distance."""

    term = TerrainLevelTerm(num_levels=3, num_envs=1, terrain_size_x=30.0)
    term.load_state_dict({**term.state_dict(), "levels": [1]})
    term.reset(torch.ones(1, dtype=torch.bool), _pose_at(0.0))
    term.update(_transition(_pose_at(2.0), [[0.0, 0.0]], transition_dt=5.0))

    assert term.progress().tolist() == [0.0]
    assert term.commanded_distances().tolist() == [0.0]
    assert term.levels().tolist() == [1]


def test_terrain_level_term_matches_curriculum_promote_demote_and_cap() -> None:
    """AC_PY_UNIT_CURRMGR_003: term outcomes match TerrainCurriculum, including cap."""

    oracle = TerrainCurriculum(num_levels=3, num_envs=1, terrain_size_x=30.0)
    term = TerrainLevelTerm(
        num_levels=3,
        num_envs=1,
        terrain_size_x=30.0,
        device="cpu",
    )
    manager = CurriculumManager({"terrain": term})
    origin = _pose_at(0.0)
    term.reset(torch.ones(1, dtype=torch.bool), origin)

    # Cap branch: commanded distance 10 → promote at 8.0; 8.1 promotes.
    metrics = manager.update(_step(8.1))
    assert oracle.update([0], [8.1], [10.0]) == (1,)
    assert tuple(term.levels().tolist()) == oracle.levels
    assert metrics["Curriculum/terrain/next_level"] == 1.0
    assert metrics["Curriculum/terrain/completed_episode_level"] == 0.0

    # Demote: progress 0 with commanded 10.
    term.reset(torch.ones(1, dtype=torch.bool), origin)
    manager.update(_step(0.0))
    assert oracle.update([0], [0.0], [10.0]) == (0,)
    assert tuple(term.levels().tolist()) == oracle.levels

    # Hold: progress between demote (<5) and promote (>8) thresholds — level stays.
    # Re-promote first so hold is checked from a non-trivial prior state.
    term.reset(torch.ones(1, dtype=torch.bool), origin)
    manager.update(_step(8.1))
    assert oracle.update([0], [8.1], [10.0]) == (1,)
    term.reset(torch.ones(1, dtype=torch.bool), origin)
    manager.update(_step(6.0))
    assert oracle.update([0], [6.0], [10.0]) == (1,)
    assert tuple(term.levels().tolist()) == oracle.levels == (1,)


def test_terrain_level_term_checkpoint_round_trip() -> None:
    """AC_PY_UNIT_CURRMGR_004: terrain term state_dict restores levels and accumulators."""

    term = TerrainLevelTerm(
        num_levels=4,
        num_envs=2,
        terrain_size_x=10.0,
        device="cpu",
    )
    manager = CurriculumManager({"terrain": term})
    term.reset(torch.ones(2, dtype=torch.bool), _pose_at(0.0, num_envs=2))
    manager.update(
        _transition(
            _pose_at(6.0, num_envs=2),
            [[1.0, 0.0], [1.0, 0.0]],
            transition_dt=1.0,
            terminated=[False, False],
            truncated=[False, False],
        )
    )
    checkpoint = manager.state_dict()
    restored = TerrainLevelTerm(
        num_levels=4,
        num_envs=2,
        terrain_size_x=10.0,
        device="cpu",
    )
    CurriculumManager({"terrain": restored}).load_state_dict(checkpoint)
    assert torch.equal(restored.levels(), term.levels())
    assert torch.equal(restored.progress(), torch.tensor([6.0, 6.0]))
    assert torch.equal(restored.commanded_distances(), term.commanded_distances())

    restored.update(
        _transition(_pose_at(7.0, num_envs=2), [[1.0, 0.0], [1.0, 0.0]], transition_dt=1.0)
    )
    assert torch.equal(restored.progress(), torch.tensor([7.0, 7.0]))


def test_ac_py_unit_terrain_008_rejects_prior_checkpoint_without_progress_state() -> None:
    """Checkpoints without the progress accumulator cannot restore this term."""

    term = TerrainLevelTerm(num_levels=2, num_envs=1, terrain_size_x=10.0)
    with pytest.raises(ValueError, match="checkpoint fields are invalid"):
        term.load_state_dict({
            "levels": [0],
            "origins": [[0.0, 0.0]],
            "elapsed": [0.0],
            "commanded_distance": [0.0],
            "rollover_rng_state": term.state_dict()["rollover_rng_state"],
        })


def test_terrain_log_keys_match_pre_migration_names() -> None:
    """AC_PY_UNIT_CURRMGR_005: Curriculum/terrain/* keys are character-identical."""

    term = TerrainLevelTerm(
        num_levels=2,
        num_envs=1,
        terrain_size_x=10.0,
    )
    metrics = CurriculumManager({"terrain": term}).update(
        _transition(_pose_at(0.0), [[0.0, 0.0]], transition_dt=1.0)
    )
    assert set(metrics) == {
        "Curriculum/terrain/next_level",
        "Curriculum/terrain/completed_episode_level",
    }


def test_ac_py_unit_dt_006_accumulates_command_distance_with_each_transition_dt() -> None:
    """Command switches use Σ(|command| × dt), including the completion frame."""

    term = TerrainLevelTerm(
        num_levels=3,
        num_envs=2,
        terrain_size_x=30.0,
        device="cpu",
    )
    manager = CurriculumManager({"terrain": term})
    term.reset(torch.ones(2, dtype=torch.bool), _pose_at(0.0, num_envs=2))

    manager.update(
        _transition(
            _pose_at(0.0, num_envs=2),
            [[1.0, 0.0], [0.0, 2.0]],
            transition_dt=0.005,
            truncated=[False, False],
        )
    )
    manager.update(
        _transition(
            _pose_at(6.0, num_envs=2),
            [[3.0, 0.0], [4.0, 0.0]],
            transition_dt=0.035,
        )
    )

    assert torch.allclose(term.elapsed(), torch.tensor([0.04, 0.04]))
    assert torch.allclose(term.commanded_distances(), torch.tensor([0.11, 0.15]))
    assert torch.equal(term.levels(), torch.tensor([1, 1], dtype=torch.uint16))

    checkpoint = manager.state_dict()
    restored = TerrainLevelTerm(
        num_levels=3,
        num_envs=2,
        terrain_size_x=30.0,
        device="cpu",
    )
    CurriculumManager({"terrain": restored}).load_state_dict(checkpoint)
    assert torch.allclose(restored.elapsed(), term.elapsed())
    assert torch.allclose(restored.commanded_distances(), term.commanded_distances())


def test_ac_py_unit_dt_006_terminated_episode_uses_command_progress() -> None:
    """An early termination promotes if its command progress crosses the threshold."""

    term = TerrainLevelTerm(
        num_levels=2,
        num_envs=1,
        terrain_size_x=10.0,
    )
    manager = CurriculumManager({"terrain": term})
    term.reset(torch.ones(1, dtype=torch.bool), _pose_at(0.0))
    manager.update(
        _transition(
            _pose_at(100.0),
            [[100.0, 0.0]],
            transition_dt=0.01,
            terminated=[True],
            truncated=[False],
        )
    )
    assert torch.equal(term.levels(), torch.ones(1, dtype=torch.uint16))


def test_ac_py_unit_dt_007_physical_failure_with_large_progress_promotes_terrain_level() -> None:
    term = TerrainLevelTerm(num_levels=3, num_envs=1, terrain_size_x=10.0)
    manager = CurriculumManager({"terrain": term})
    term.reset(torch.ones(1, dtype=torch.bool), _pose_at(0.0))
    manager.update(_step(6.0))
    assert term.levels().tolist() == [1]

    term.reset(torch.ones(1, dtype=torch.bool), _pose_at(0.0))
    manager.update(_step(100.0, terminated=True))

    assert term.levels().tolist() == [2]

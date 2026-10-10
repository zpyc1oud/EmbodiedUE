"""Literal trajectories prove that physical acceptance rejects named faults."""
from __future__ import annotations

import math

import pytest

from tests.e2e.support.physical_oracles import force_trajectory_matches, joint_consistency


@pytest.mark.parametrize("fault", ["none", "omitted", "reversed", "units", "stale", "swapped"])
def test_same_force_oracle_rejects_input_and_routing_faults(fault: str) -> None:
    # dt=.1; masses 1 and 2 kg. Each row is the next declared physical command.
    forces = [[1.0, -4.0], [0.0, 0.0], [-1.0, 4.0]]
    measured = [[0.0, 0.0], [0.1, -0.2], [0.1, -0.2], [0.0, 0.0]]
    if fault in {"omitted", "reversed", "units"}:
        scale = {"omitted": 0.0, "reversed": -1.0, "units": 0.01}[fault]
        measured = [[scale * x for x in row] for row in measured]
    elif fault == "stale":
        measured = [[0.0, 0.0], [0.1, -0.2], [0.2, -0.4], [0.1, -0.2]]
    elif fault == "swapped":
        measured = [list(reversed(row)) for row in measured]
    assert force_trajectory_matches(measured, forces, [1.0, 2.0], physics_dt=0.1) is (fault == "none")


def test_position_and_velocity_literals_accept_constant_acceleration() -> None:
    result = joint_consistency([0, 0.1, 0.2, 0.3], [0, 0.01, 0.04, 0.09],
                               [0, 0.2, 0.4, 0.6], physics_dt=0.1, absolute_budget=1e-9)
    assert result.displacement == pytest.approx(0.09)
    assert result.integrated_velocity == pytest.approx(0.09)
    assert result.passed


@pytest.mark.parametrize("velocity", [[0.2, 0.4, 0.6, 0.8], [0, -0.2, -0.4, -0.6], [0, 20, 40, 60]])
def test_same_joint_oracle_rejects_one_step_lag_sign_and_unit_errors(velocity: list[float]) -> None:
    result = joint_consistency([0, 0.1, 0.2, 0.3], [0, 0.01, 0.04, 0.09],
                               velocity, physics_dt=0.1, absolute_budget=1e-9)
    assert not result.passed


def test_stationary_position_with_persistent_velocity_is_not_consistent() -> None:
    result = joint_consistency([0, 0.005, 0.01, 0.015], [0.2] * 4, [-0.8] * 4, physics_dt=0.005)
    assert result.displacement == pytest.approx(0)
    assert result.integrated_velocity == pytest.approx(-0.012)
    assert not result.passed


def test_joint_angle_wrap_is_not_a_two_pi_jump() -> None:
    result = joint_consistency([0, 0.1, 0.2], [math.pi - 0.1, math.pi, -math.pi + 0.1],
                               [1, 1, 1], physics_dt=0.1)
    assert result.displacement == pytest.approx(0.2)
    assert result.passed


def test_opposite_velocity_errors_cannot_cancel_at_the_end() -> None:
    # The reported angle never moves. Wrong +1/-1 rad/s feedback integrates
    # to zero over the complete capture, but is wrong in each half.
    times = [index * 0.005 for index in range(201)]
    velocities = [1.0] * 100 + [0.0] + [-1.0] * 100
    result = joint_consistency(times, [0.2] * len(times), velocities, physics_dt=0.005)
    assert abs(result.error) < 1e-12
    assert not result.passed


def test_later_velocity_variation_cannot_relax_an_earlier_failure() -> None:
    # A later noisy section must not increase the earlier prefix's budget.
    times = [index * 0.005 for index in range(221)]
    velocities = [1.0] * 100 + [0.0] + [-1.0] * 100 + [100.0, -100.0] * 10
    result = joint_consistency(times, [0.2] * len(times), velocities, physics_dt=0.005)
    assert not result.passed


@pytest.mark.parametrize("times", [[0, 0.1], [0, 0.2, 0.3], [0, 0.1, 0.1], [0.2, 0.1, 0], [0, math.nan, 0.2]])
def test_incomplete_or_invalid_solver_trace_fails(times: list[float]) -> None:
    with pytest.raises(ValueError):
        joint_consistency(times, [0, 0, 0], [0, 0, 0], physics_dt=0.1)


@pytest.mark.parametrize("measured", [[[0], [0.1]], [[0, 0], [math.nan, 0]], [[0, 0]]])
def test_incomplete_or_nonfinite_force_trace_fails(measured: list[list[float]]) -> None:
    with pytest.raises(ValueError):
        force_trajectory_matches(measured, [[1, 0]], [1, 2], physics_dt=0.1)


def test_unmarked_position_jump_fails_the_same_consistency_oracle() -> None:
    result = joint_consistency([0, 0.005, 0.01, 0.015], [0.0, 0.0, 0.05, 0.05],
                               [0.0] * 4, physics_dt=0.005)
    assert not result.passed


@pytest.mark.parametrize("field", ["position", "velocity"])
@pytest.mark.parametrize("invalid", [math.nan, math.inf, -math.inf])
def test_nonfinite_joint_feedback_is_rejected(field: str, invalid: float) -> None:
    positions = [0.0, 0.0, 0.0]
    velocities = [0.0, 0.0, 0.0]
    (positions if field == "position" else velocities)[1] = invalid
    with pytest.raises(ValueError, match="nonfinite"):
        joint_consistency([0, 0.005, 0.01], positions, velocities, physics_dt=0.005)

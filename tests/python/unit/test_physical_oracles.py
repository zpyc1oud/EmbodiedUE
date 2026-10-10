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


@pytest.mark.parametrize("times", [[0, 0.1], [0, 0.2, 0.3], [0, 0.1, 0.1], [0.2, 0.1, 0], [0, math.nan, 0.2]])
def test_incomplete_or_invalid_solver_trace_fails(times: list[float]) -> None:
    with pytest.raises(ValueError):
        joint_consistency(times, [0, 0, 0], [0, 0, 0], physics_dt=0.1)


@pytest.mark.parametrize("measured", [[[0], [0.1]], [[0, 0], [math.nan, 0]], [[0, 0]]])
def test_incomplete_or_nonfinite_force_trace_fails(measured: list[list[float]]) -> None:
    with pytest.raises(ValueError):
        force_trajectory_matches(measured, [[1, 0]], [1, 2], physics_dt=0.1)

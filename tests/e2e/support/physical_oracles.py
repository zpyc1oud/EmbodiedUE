"""Independent SI acceptance calculations for completed physical traces.

These functions consume measurements. They do not invoke product conversion,
kinematics, drive-law, or observation code. Their unit tests are sensitivity
checks; physical acceptance still requires a real UE trace.
"""
from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass


@dataclass(frozen=True)
class JointConsistency:
    """Signed position change, integrated velocity, and a fixed error budget."""

    displacement: float
    integrated_velocity: float
    error: float
    tolerance: float

    @property
    def passed(self) -> bool:
        return abs(self.error) <= self.tolerance


def joint_consistency(
    times: Sequence[float], positions: Sequence[float], velocities: Sequence[float],
    *, physics_dt: float, absolute_budget: float = 0.008,
) -> JointConsistency:
    """Compare unwrapped displacement with trapezoidal velocity integration.

    Every row must belong to the next completed solver step. The integration
    allowance is dt/2 times measured velocity variation; it cannot conceal a
    persistent velocity offset in a stationary joint. A separate absolute
    allowance accounts for accumulated pose readback error.
    """
    if not (len(times) == len(positions) == len(velocities) and len(times) >= 3):
        raise ValueError("A complete trace needs at least three aligned samples")
    if not math.isfinite(physics_dt) or physics_dt <= 0:
        raise ValueError("physics_dt must be positive and finite")
    if not math.isfinite(absolute_budget) or absolute_budget < 0:
        raise ValueError("absolute_budget must be finite and nonnegative")
    if any(not math.isfinite(x) for values in (times, positions, velocities) for x in values):
        raise ValueError("Physical trace contains nonfinite data")
    displacement = integrated = variation = 0.0
    for index in range(1, len(times)):
        dt = times[index] - times[index - 1]
        if not math.isclose(dt, physics_dt, rel_tol=1e-6, abs_tol=1e-9):
            raise ValueError("Missing, repeated, or reordered completed solver step")
        change = positions[index] - positions[index - 1]
        displacement += math.atan2(math.sin(change), math.cos(change))
        integrated += (velocities[index] + velocities[index - 1]) * (dt / 2)
        variation += abs(velocities[index] - velocities[index - 1])
    return JointConsistency(displacement, integrated, displacement - integrated,
                            absolute_budget + physics_dt * variation / 2)


def force_trajectory_matches(
    velocities: Sequence[Sequence[float]], forces: Sequence[Sequence[float]],
    masses: Sequence[float], *, physics_dt: float,
    absolute_budget: float = 1e-4, relative_budget: float = 0.005,
) -> bool:
    """Check every body's velocity after every independently declared force.

    The first velocity row is the initial condition. Each force row contains
    the force applied during the following solver step, one scalar axis per
    named body. Callers fix the body order before they submit the commands.
    """
    if len(velocities) != len(forces) + 1 or not forces or not masses:
        raise ValueError("Force trace has incomplete initial or terminal state")
    if not math.isfinite(physics_dt) or physics_dt <= 0:
        raise ValueError("physics_dt must be positive and finite")
    if any(not math.isfinite(m) or m <= 0 for m in masses):
        raise ValueError("Masses must be positive and finite")
    if any(not math.isfinite(x) or x < 0 for x in (absolute_budget, relative_budget)):
        raise ValueError("Error budgets must be finite and nonnegative")
    rows = (*velocities, *forces)
    if any(len(row) != len(masses) for row in rows):
        raise ValueError("Force and state rows must preserve the declared body order")
    if any(not math.isfinite(x) for row in rows for x in row):
        raise ValueError("Physical trace contains nonfinite data")
    expected = list(velocities[0])
    for command, actual in zip(forces, velocities[1:], strict=True):
        for body, mass in enumerate(masses):
            expected[body] += command[body] * physics_dt / mass
            if abs(actual[body] - expected[body]) > absolute_budget + relative_budget * abs(expected[body]):
                return False
    return True

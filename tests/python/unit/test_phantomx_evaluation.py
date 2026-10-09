"""Measure tracking and survival independently through the evaluator interface."""

from pathlib import Path

import pytest
import torch

from uerl.tasks.evaluation import EvaluationSummary
from uerl.tasks.phantomx.evaluation import PhantomXEvaluator, format_phantomx_evaluation


def _observe(
    evaluator: PhantomXEvaluator,
    *,
    dt: float,
    errors: list[float],
    yaw_errors: list[float],
    done: list[bool],
    contact: list[bool],
    timeout: list[bool] | None = None,
) -> None:
    batch = len(errors)
    values = {
        "forward_velocity": [0.0] * batch,
        "linear_velocity_error": errors,
        "yaw_rate_error": yaw_errors,
        "command_vx": [0.45] * batch,
        "command_speed": [0.45] * batch,
        "action_clip_fraction": [0.0] * batch,
        "torque_clip_fraction": [0.0] * batch,
        "torque_over_limit": [0.0] * batch,
        "action_rate": [0.0] * batch,
        "body_height": [0.18] * batch,
        "base_contact": contact,
        "timeout": done if timeout is None else timeout,
        "reset_age_steps": [1.0] * batch,
        "body_clearance": [0.18] * batch,
        "upright": [1.0] * batch,
        "reset_event": [0.0] * batch,
        "reset_joint_error_max": [0.0] * batch,
    }
    metrics: dict[str, object] = {f"phantomx/{key}": torch.tensor(value) for key, value in values.items()}
    metrics["transition_dt_s"] = dt
    evaluator.observe(
        {"policy": torch.zeros(batch, 12)},
        torch.zeros(batch, 18),
        metrics,
        torch.zeros(batch),
        torch.tensor(done),
        torch.ones(batch),
    )


def _summary(steps: int, episodes: int) -> EvaluationSummary:
    return EvaluationSummary("UERL-PhantomX-Walk-v0", Path("model.pt"), steps, episodes, 20.0, 0.0)


def test_stationary_timeout_reports_survival_without_claiming_tracking_success() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    _observe(evaluator, dt=0.02, errors=[0.45, 0.45], yaw_errors=[0.0, 0.0], done=[True, True], contact=[False, True])
    result = evaluator.finish(_summary(1, 2))
    assert result.survival_rate == pytest.approx(0.5)
    assert result.linear_velocity_rmse == pytest.approx(0.45)
    assert result.fall_rate == pytest.approx(0.5)
    text = format_phantomx_evaluation(result)
    assert "survival_rate=0.500000" in text
    assert "success_rate=" not in text


def test_tracking_errors_use_physical_time_and_all_distinct_rows() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    _observe(evaluator, dt=0.005, errors=[0.0, 0.4], yaw_errors=[0.0, 0.8], done=[False, False], contact=[False, False])
    _observe(evaluator, dt=0.035, errors=[0.2, 0.2], yaw_errors=[0.4, 0.4], done=[True, False], contact=[False, False])
    result = evaluator.finish(_summary(2, 1))
    # 5 ms contributes 1/8 of measured time. Squared row means are .08 and .04.
    assert result.mean_speed_error == pytest.approx(0.2)
    assert result.linear_velocity_rmse == pytest.approx(0.045**0.5)
    assert result.yaw_rate_rmse == pytest.approx(0.18**0.5)
    assert result.measured_seconds == pytest.approx(0.04)
    assert result.survival_rate == pytest.approx(1.0)


def test_partial_episodes_do_not_enter_survival_denominator() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    _observe(
        evaluator,
        dt=0.02,
        errors=[0.1, 0.3, 0.5],
        yaw_errors=[0.2, 0.4, 0.6],
        done=[False, False, False],
        contact=[False, False, False],
    )
    result = evaluator.finish(_summary(1, 0))
    assert result.completed_episodes == 0
    assert result.termination_events == ()
    assert result.survival_rate == 0.0
    assert result.fall_rate == 0.0
    assert result.linear_velocity_rmse == pytest.approx((0.35 / 3) ** 0.5)
    assert result.yaw_rate_rmse == pytest.approx((0.56 / 3) ** 0.5)


def test_sparse_termination_keeps_slot_identity_and_counts_repeated_episodes() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    _observe(
        evaluator,
        dt=0.01,
        errors=[0.1, 0.2, 0.3],
        yaw_errors=[0.2, 0.3, 0.4],
        done=[False, True, True],
        contact=[False, False, True],
    )
    # Slot 1 completes a second timeout. Slot 0 remains a partial episode.
    _observe(
        evaluator,
        dt=0.03,
        errors=[0.4, 0.5, 0.6],
        yaw_errors=[0.5, 0.6, 0.7],
        done=[False, True, False],
        contact=[False, False, False],
    )
    result = evaluator.finish(_summary(2, 3))
    assert result.survival_rate == pytest.approx(2 / 3)
    assert result.fall_rate == pytest.approx(1 / 3)
    assert [(event["slot"], event["termination_reason"]) for event in result.termination_events] == [
        (1, "timeout"),
        (2, "base_contact"),
        (1, "timeout"),
    ]
    assert result.mean_speed_error == pytest.approx(0.425)
    assert result.linear_velocity_rmse == pytest.approx((0.14 / 12 + 0.77 / 4) ** 0.5)


def test_completed_slot_without_reason_fails_instead_of_counting_survival() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    with pytest.raises(RuntimeError, match="without a Task termination reason"):
        _observe(evaluator, dt=0.02, errors=[0.0], yaw_errors=[0.0], done=[True], contact=[False], timeout=[False])

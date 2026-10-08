"""Measure learned tracking separately from surviving a locomotion episode."""

from pathlib import Path

import pytest
import torch

from uerl.tasks.evaluation import EvaluationSummary
from uerl.tasks.phantomx.evaluation import PhantomXEvaluator


def test_locomotion_tracking_success_rejects_surviving_without_motion() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    observations = {"policy": torch.zeros(3, 116)}
    observations["policy"][:, 9] = 0.1
    for dt, done in ((0.005, False), (0.035, True)):
        metrics: dict[str, object] = {
            "transition_dt_s": dt,
            "runtime/state_valid": torch.ones(3, dtype=torch.bool),
            "runtime/slot_fault_code": torch.zeros(3, dtype=torch.long),
        }
        zero_names = (
            "action_clip_fraction",
            "torque_clip_fraction",
            "torque_over_limit",
            "reset_joint_error_max",
            "action_rate",
            "yaw_rate_error",
        )
        metrics.update({f"phantomx/{name}": torch.zeros(3) for name in zero_names})
        metrics.update(
            {
                "phantomx/forward_velocity": torch.tensor([0.0, 0.1, 0.1]),
                "phantomx/linear_velocity_error": torch.tensor([0.1, 0.0, 0.0]),
                "phantomx/command_vx": torch.full((3,), 0.1),
                "phantomx/command_speed": torch.full((3,), 0.1),
                "phantomx/body_height": torch.full((3,), 0.18),
                "phantomx/body_clearance": torch.full((3,), 0.18),
                "phantomx/upright": torch.ones(3),
                "phantomx/reset_age_steps": torch.full((3,), 2.0 if done else 1.0),
                "phantomx/reset_event": torch.full((3,), not done),
                "Episode_Termination/base_contact": torch.tensor([False, False, done]),
                "Episode_Termination/timeout": torch.tensor([done, done, False]),
            }
        )
        evaluator.observe(
            observations, torch.zeros(3, 18), metrics, torch.zeros(3), torch.full((3,), done), torch.full((3,), 2)
        )
    result = evaluator.finish(EvaluationSummary("Walk", Path("model.pt"), 2, 3, 2.0, 0.0))
    assert result.survival_rate == pytest.approx(2 / 3)
    assert result.tracking_success_rate == pytest.approx(1 / 3)
    assert result.linear_velocity_rmse == pytest.approx(0.0577350269)
    assert result.yaw_rate_rmse == 0.0


def _metrics(linear: list[float], yaw: list[float], *, dt: float) -> dict[str, object]:
    """Use measured-error fixtures, without invoking production reward helpers."""
    count = len(linear)
    metrics: dict[str, object] = {
        "transition_dt_s": dt,
        "state_valid": torch.ones(count, dtype=torch.bool),
        "slot_fault_code": torch.zeros(count, dtype=torch.long),
    }
    for name in (
        "action_clip_fraction",
        "torque_clip_fraction",
        "torque_over_limit",
        "reset_joint_error_max",
        "action_rate",
        "reset_event",
        "reset_age_steps",
        "body_clearance",
        "body_height",
        "upright",
        "base_contact",
        "timeout",
        "forward_velocity",
    ):
        metrics[name] = torch.zeros(count)
    metrics.update(
        {
            "command_vx": torch.full((count,), 0.1),
            "command_speed": torch.full((count,), 0.1),
            "linear_velocity_error": torch.tensor(linear),
            "yaw_rate_error": torch.tensor(yaw),
        }
    )
    return metrics


def test_tracking_uses_physical_time_and_preserves_unfinished_slot_history() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    obs = {"policy": torch.zeros(2, 116)}
    obs["policy"][:, 9] = 0.1
    first = _metrics([0.08, 0.08], [0.0, 0.0], dt=0.005)
    first["timeout"] = torch.tensor([True, False])
    evaluator.observe(obs, torch.zeros(2, 18), first, torch.zeros(2), torch.tensor([True, False]), torch.tensor([1, 0]))
    second = _metrics([0.0, 0.04], [0.0, 0.0], dt=0.035)
    second["timeout"] = torch.ones(2, dtype=torch.bool)
    evaluator.observe(
        obs, torch.zeros(2, 18), second, torch.zeros(2), torch.ones(2, dtype=torch.bool), torch.tensor([1, 2])
    )
    report = evaluator.finish(EvaluationSummary("Walk", Path("policy.pt"), 2, 3, 4 / 3, 0.0))
    # Slot 1: (0.08²*0.005 + 0.04²*0.035)/0.04 = 0.0022 < 0.05².
    # An unweighted mean would fail. Slot 0's first episode still fails.
    assert report.tracking_success_rate == pytest.approx(2 / 3)
    assert report.linear_velocity_rmse == pytest.approx((0.00012 / 0.08) ** 0.5)
    assert [e["tracking_success"] for e in report.termination_events] == [False, True, True]


def test_invalid_sample_is_excluded_but_its_episode_cannot_pass() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    obs = {"policy": torch.zeros(2, 116)}
    obs["policy"][:, 9] = 0.1
    metrics = _metrics([float("nan"), 0.02], [float("nan"), 0.01], dt=0.02)
    metrics["state_valid"] = torch.tensor([False, True])
    metrics["slot_fault_code"] = torch.tensor([1, 0])
    metrics["timeout"] = torch.ones(2, dtype=torch.bool)
    metrics["base_contact"] = torch.tensor([True, False])
    evaluator.observe(obs, torch.zeros(2, 18), metrics, torch.zeros(2), torch.ones(2, dtype=torch.bool), torch.ones(2))
    report = evaluator.finish(EvaluationSummary("Walk", Path("policy.pt"), 1, 2, 1.0, 0.0))
    assert report.fault_rate == 0.5
    assert report.fall_rate == 0.0
    assert report.tracking_success_rate == 0.5
    assert report.linear_velocity_rmse == pytest.approx(0.02)
    assert report.yaw_rate_rmse == pytest.approx(0.01)


def test_turning_error_can_fail_an_otherwise_successful_episode() -> None:
    evaluator = PhantomXEvaluator(sample_hz=50.0)
    obs = {"policy": torch.zeros(1, 116)}
    obs["policy"][:, 11] = 0.2
    metrics = _metrics([0.0], [0.2], dt=0.02)
    metrics["command_speed"] = torch.zeros(1)
    metrics["command_vx"] = torch.zeros(1)
    metrics["timeout"] = torch.ones(1, dtype=torch.bool)
    evaluator.observe(obs, torch.zeros(1, 18), metrics, torch.zeros(1), torch.ones(1, dtype=torch.bool), torch.ones(1))
    report = evaluator.finish(EvaluationSummary("Walk", Path("policy.pt"), 1, 1, 1.0, 0.0))
    assert report.survival_rate == 1.0
    assert report.tracking_success_rate == 0.0
    assert report.yaw_rate_rmse == pytest.approx(0.2)

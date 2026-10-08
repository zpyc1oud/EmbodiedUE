"""Catch evaluation must distinguish contact success, timeout, fall and faults."""

from pathlib import Path

import pytest
import torch

from uerl.tasks.evaluation import EvaluationSummary
from uerl.tasks.phantomx.catch_evaluation import CatchEvaluator


def test_catch_evaluation_does_not_count_timeout_or_fault_as_capture() -> None:
    evaluator = CatchEvaluator(sample_hz=50.0)
    evaluator.observe(
        observations={}, actions=torch.zeros(4, 1), rewards=torch.zeros(4),
        dones=torch.ones(4, dtype=torch.bool), terminal_episode_lengths=torch.ones(4),
        metrics={
            "Episode_Termination/capture": torch.tensor([1.0, 1.0, 0.0, 1.0]),
            "Episode_Termination/base_contact": torch.tensor([0.0, 1.0, 0.0, 0.0]),
            "Episode_Termination/timeout": torch.tensor([0.0, 0.0, 1.0, 0.0]),
            "runtime/state_valid": torch.tensor([True, True, True, False]),
            "runtime/slot_fault_code": torch.tensor([0, 0, 0, 2]),
            "phantomx/linear_velocity_error": torch.tensor([0.0, 0.1, 0.2, 1000.0]),
            "phantomx/yaw_rate_error": torch.tensor([0.0, 0.2, 0.4, 1000.0]),
            "phantomx/action_clip_fraction": torch.tensor([0.0, 0.3, 0.6, 1.0]),
            "transition_dt_s": 0.02,
        },
    )
    report = evaluator.finish(EvaluationSummary("Catch", Path("model.pt"), 1, 4, 1.0, 0.0))
    assert report.capture_rate == 0.25
    assert report.fall_rate == 0.25
    assert report.timeout_rate == 0.25
    assert report.fault_rate == 0.25
    assert report.mean_capture_time_s == 0.02
    assert report.linear_velocity_rmse == pytest.approx(0.1290994449)
    assert report.yaw_rate_rmse == pytest.approx(0.2581988897)
    assert report.mean_action_clip_fraction == pytest.approx(0.3)


def test_catch_evaluation_weights_time_and_resets_only_completed_slots() -> None:
    evaluator = CatchEvaluator(sample_hz=50.0)
    for dt, errors, captured, clips in (
        (0.005, [0.8, 0.0], [True, False], [0.0, 0.2]),
        (0.035, [0.0, 0.8], [False, True], [0.4, 0.6]),
    ):
        evaluator.observe(
            observations={}, actions=torch.zeros(2, 1), rewards=torch.zeros(2),
            dones=torch.tensor(captured), terminal_episode_lengths=torch.ones(2),
            metrics={
                "Episode_Termination/capture": torch.tensor(captured),
                "Episode_Termination/base_contact": torch.zeros(2),
                "Episode_Termination/timeout": torch.zeros(2),
                "runtime/state_valid": torch.ones(2, dtype=torch.bool),
                "runtime/slot_fault_code": torch.zeros(2, dtype=torch.long),
                "phantomx/linear_velocity_error": torch.tensor(errors),
                "phantomx/yaw_rate_error": torch.zeros(2),
                "phantomx/action_clip_fraction": torch.tensor(clips),
                "transition_dt_s": dt,
            },
        )
    report = evaluator.finish(EvaluationSummary("Catch", Path("model.pt"), 2, 2, 1.5, 0.0))
    assert report.capture_rate == 1.0
    assert report.mean_capture_time_s == pytest.approx(0.0225)
    assert report.linear_velocity_rmse == pytest.approx(0.5656854249)
    assert report.yaw_rate_rmse == 0.0
    assert report.mean_action_clip_fraction == pytest.approx(0.45)

"""Verify PhantomX generalization metric aggregation."""

from __future__ import annotations

from pathlib import Path

import pytest
import torch

from uerl.tasks.evaluation import EvaluationSummary
from uerl.tasks.phantomx.generalization import (
    GENERALIZATION_NON_FOOT_CONTACT_METRIC,
    GENERALIZATION_PURSUIT_DISTANCE_METRIC,
    GENERALIZATION_ROOT_HEIGHT_METRIC,
    GeneralizationEvaluator,
)


def _summary(*, steps: int, completed_episodes: int) -> EvaluationSummary:
    return EvaluationSummary(
        task_id="UERL-PhantomX-Pursuit-v0",
        checkpoint=Path("model.pt"),
        steps=steps,
        completed_episodes=completed_episodes,
        mean_episode_length=0.0,
        mean_reward=0.0,
    )


def _observe(
    evaluator: GeneralizationEvaluator,
    *,
    root_height: list[float],
    pursuit_distance: list[float],
    non_foot_contact: list[float],
    done: list[bool],
    terminal_length: list[int],
) -> None:
    metrics = {
        GENERALIZATION_ROOT_HEIGHT_METRIC: torch.tensor(root_height),
        GENERALIZATION_PURSUIT_DISTANCE_METRIC: torch.tensor(pursuit_distance),
        GENERALIZATION_NON_FOOT_CONTACT_METRIC: torch.tensor(non_foot_contact),
    }
    evaluator.observe(
        observations={},
        actions=torch.zeros(len(root_height), 18),
        metrics=metrics,
        rewards=torch.zeros(len(root_height)),
        dones=torch.tensor(done),
        terminal_episode_lengths=torch.tensor(terminal_length),
    )


def test_aggregates_success_height_and_contact_over_completed_episode() -> None:
    evaluator = GeneralizationEvaluator(sample_hz=50.0)
    _observe(
        evaluator,
        root_height=[0.20],
        pursuit_distance=[2.0],
        non_foot_contact=[0.0],
        done=[False],
        terminal_length=[0],
    )
    _observe(
        evaluator,
        root_height=[0.18],
        pursuit_distance=[1.0],
        non_foot_contact=[1.0],
        done=[False],
        terminal_length=[0],
    )
    _observe(
        evaluator,
        root_height=[0.16],
        pursuit_distance=[1.5],
        non_foot_contact=[0.0],
        done=[True],
        terminal_length=[3],
    )

    result = evaluator.finish(_summary(steps=3, completed_episodes=1))

    assert result.completed_episodes == 1
    assert result.survival_seconds == pytest.approx(3 / 50)
    assert result.pursuit_success_rate == pytest.approx(1.0)
    assert result.mean_root_height == pytest.approx(0.18)
    assert result.contact_violation_rate == pytest.approx(1 / 3)


def test_includes_right_censored_active_slots_in_survival_denominator() -> None:
    evaluator = GeneralizationEvaluator(sample_hz=10.0)
    _observe(
        evaluator,
        root_height=[0.20, 0.22],
        pursuit_distance=[1.0, 2.0],
        non_foot_contact=[1.0, 0.0],
        done=[False, False],
        terminal_length=[0, 0],
    )
    _observe(
        evaluator,
        root_height=[0.18, 0.20],
        pursuit_distance=[2.0, 1.0],
        non_foot_contact=[1.0, 0.0],
        done=[True, False],
        terminal_length=[2, 0],
    )

    result = evaluator.finish(_summary(steps=2, completed_episodes=1))

    assert result.survival_seconds == pytest.approx(0.2)
    assert result.pursuit_success_rate == pytest.approx(1.0)
    assert result.mean_root_height == pytest.approx(0.20)
    assert result.contact_violation_rate == pytest.approx(0.5)

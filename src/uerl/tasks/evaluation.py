"""Small Task-owned evaluation interfaces and generic rollout results."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

import torch


@dataclass(frozen=True, slots=True)
class EvaluationSummary:
    """Common rollout facts available to every Task evaluator."""

    task_id: str
    checkpoint: Path
    steps: int
    completed_episodes: int
    mean_episode_length: float
    mean_reward: float


class TaskEvaluator(Protocol):
    """Consume rollout data and produce one Task-owned evaluation result."""

    def observe(
        self,
        observations: Mapping[str, torch.Tensor],
        actions: torch.Tensor,
        metrics: Mapping[str, object],
        rewards: torch.Tensor,
        dones: torch.Tensor,
        terminal_episode_lengths: torch.Tensor,
    ) -> None:
        """Consume one post-step rollout observation without changing control flow."""

    def finish(self, summary: EvaluationSummary) -> object:
        """Build the Task-owned result from the common rollout summary."""


def format_generic_evaluation(result: EvaluationSummary) -> str:
    """Format a Task-neutral evaluation result for CLI output."""

    return (
        f"[VERIFY] task={result.task_id} checkpoint={result.checkpoint} steps={result.steps} "
        f"episodes={result.completed_episodes} episode_length={result.mean_episode_length:.3f} "
        f"mean_reward={result.mean_reward:.6f} shutdown=PASS"
    )


__all__ = [
    "EvaluationSummary",
    "TaskEvaluator",
    "format_generic_evaluation",
]

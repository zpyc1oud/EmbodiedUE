"""Measure contact capture separately from survival and failed episodes."""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import torch

from uerl.tasks.evaluation import EvaluationSummary


@dataclass(frozen=True, slots=True)
class CatchEvaluationResult:
    task_id: str
    checkpoint: Path
    steps: int
    completed_episodes: int
    capture_rate: float
    fall_rate: float
    timeout_rate: float
    fault_rate: float
    mean_capture_time_s: float
    linear_velocity_rmse: float
    yaw_rate_rmse: float
    mean_action_clip_fraction: float


class CatchEvaluator:
    def __init__(self, sample_hz: float) -> None:
        del sample_hz  # Evaluation uses the actual completed physical interval.
        self._elapsed: torch.Tensor | None = None
        self._capture_count = 0
        self._fall_count = 0
        self._timeout_count = 0
        self._fault_count = 0
        self._capture_seconds = 0.0
        self._valid_seconds = 0.0
        self._linear_error_integral = 0.0
        self._yaw_error_integral = 0.0
        self._clip_integral = 0.0

    def observe(
        self, observations: Mapping[str, torch.Tensor], actions: torch.Tensor,
        metrics: Mapping[str, object], rewards: torch.Tensor, dones: torch.Tensor,
        terminal_episode_lengths: torch.Tensor,
    ) -> None:
        del observations, actions, rewards, terminal_episode_lengths
        dt = float(cast(float, metrics["transition_dt_s"]))
        if not math.isfinite(dt) or dt <= 0.0:
            raise ValueError("Catch evaluation requires a positive completed interval")
        completed = dones.bool()
        if self._elapsed is None:
            self._elapsed = torch.zeros_like(dones, dtype=torch.float64)
        self._elapsed += dt
        valid = _tensor(metrics, "runtime/state_valid").bool() & _tensor(metrics, "runtime/slot_fault_code").eq(0)
        fault = completed & ~valid
        fall = completed & valid & _tensor(metrics, "Episode_Termination/base_contact").bool()
        capture = completed & valid & ~fall & _tensor(metrics, "Episode_Termination/capture").bool()
        timeout = completed & valid & ~fall & ~capture & _tensor(metrics, "Episode_Termination/timeout").bool()
        if bool((completed & ~(fault | fall | capture | timeout)).any()):
            raise ValueError("Catch completed without a capture, fall, timeout or Slot fault")
        self._capture_count += int(capture.sum().item())
        self._fall_count += int(fall.sum().item())
        self._timeout_count += int(timeout.sum().item())
        self._fault_count += int(fault.sum().item())
        self._capture_seconds += float(self._elapsed[capture].sum().item())
        self._elapsed[completed] = 0.0
        self._valid_seconds += int(valid.sum().item()) * dt
        linear_error_squared = _tensor(metrics, "phantomx/linear_velocity_error")[valid].square()
        self._linear_error_integral += float(linear_error_squared.sum()) * dt
        self._yaw_error_integral += float(_tensor(metrics, "phantomx/yaw_rate_error")[valid].square().sum()) * dt
        self._clip_integral += float(_tensor(metrics, "phantomx/action_clip_fraction")[valid].sum()) * dt

    def finish(self, summary: EvaluationSummary) -> CatchEvaluationResult:
        completed = summary.completed_episodes
        counted = self._capture_count + self._fall_count + self._timeout_count + self._fault_count
        if counted != completed:
            raise ValueError("Catch evaluation episode counts disagree with rollout summary")
        duration = self._valid_seconds
        return CatchEvaluationResult(
            summary.task_id, summary.checkpoint, summary.steps, completed,
            self._capture_count / completed if completed else 0.0,
            self._fall_count / completed if completed else 0.0,
            self._timeout_count / completed if completed else 0.0,
            self._fault_count / completed if completed else 0.0,
            self._capture_seconds / self._capture_count if self._capture_count else 0.0,
            math.sqrt(self._linear_error_integral / duration) if duration else math.nan,
            math.sqrt(self._yaw_error_integral / duration) if duration else math.nan,
            self._clip_integral / duration if duration else math.nan,
        )


def _tensor(metrics: Mapping[str, object], name: str) -> torch.Tensor:
    value = metrics[name]
    if not isinstance(value, torch.Tensor):
        raise TypeError(f"Catch metric {name} must be a tensor")
    return value.reshape(-1)


def format_catch_evaluation(result: object) -> str:
    report = cast(CatchEvaluationResult, result)
    return (
        f"[VERIFY] task={report.task_id} checkpoint={report.checkpoint} steps={report.steps} "
        f"episodes={report.completed_episodes} capture_rate={report.capture_rate:.6f} "
        f"fall_rate={report.fall_rate:.6f} timeout_rate={report.timeout_rate:.6f} fault_rate={report.fault_rate:.6f} "
        f"capture_time_s={report.mean_capture_time_s:.6f} linear_rmse={report.linear_velocity_rmse:.6f} "
        f"yaw_rmse={report.yaw_rate_rmse:.6f} action_clip_fraction={report.mean_action_clip_fraction:.6f} shutdown=PASS"
    )

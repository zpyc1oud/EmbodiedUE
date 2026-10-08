"""PhantomX-owned evaluation aggregation and CLI formatting."""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import cast, final

import torch
from typing_extensions import override

from ..evaluation import EvaluationSummary, TaskEvaluator


@dataclass(frozen=True, slots=True)
class PhantomXEvaluationResult:
    """Report PhantomX performance and the active termination reasons."""

    task_id: str
    checkpoint: Path
    steps: int
    completed_episodes: int
    mean_forward_velocity: float
    mean_speed_error: float
    survival_rate: float
    tracking_success_rate: float
    linear_velocity_rmse: float
    yaw_rate_rmse: float
    fault_rate: float
    fall_rate: float
    base_contact_rate: float
    mean_episode_length: float
    mean_command_vx: float
    mean_command_speed: float
    mean_actor_command_vx: float
    mean_actor_command_speed: float
    mean_command_observation_error: float
    mean_action_clip_fraction: float
    mean_torque_clip_fraction: float
    mean_torque_over_limit: float
    mean_reset_joint_error: float
    mean_action_rate: float
    mean_body_height: float
    dominant_body_height_frequency_hz: float
    termination_events: tuple[Mapping[str, object], ...] = ()


@final
class PhantomXEvaluator(TaskEvaluator):
    """Use physical-time errors and distinguish survival from command tracking."""

    def __init__(self, sample_hz: float) -> None:
        self._sample_hz = sample_hz
        self._sums: dict[str, float] = {}
        self._valid_seconds = 0.0
        self._linear_squared = 0.0
        self._yaw_squared = 0.0
        self._episode_seconds: torch.Tensor | None = None
        self._episode_linear: torch.Tensor | None = None
        self._episode_yaw: torch.Tensor | None = None
        self._episode_fault: torch.Tensor | None = None
        self._survived = 0
        self._tracked = 0
        self._falls = 0
        self._faults = 0
        self._reset_joint_error_sum = 0.0
        self._reset_joint_error_count = 0
        self._body_height_series: list[float] = []
        self._body_height_dts: list[float] = []
        self._pending_height_dt = 0.0
        self._termination_events: list[Mapping[str, object]] = []

    @override
    def observe(
        self,
        observations: Mapping[str, torch.Tensor],
        actions: torch.Tensor,
        metrics: Mapping[str, object],
        rewards: torch.Tensor,
        dones: torch.Tensor,
        terminal_episode_lengths: torch.Tensor,
    ) -> None:
        del actions, rewards, terminal_episode_lengths
        dt = float(cast(float, metrics["transition_dt_s"]))
        if not math.isfinite(dt) or dt <= 0.0:
            raise ValueError("PhantomX evaluation requires a positive completed interval")
        completed = dones.bool()
        valid = _task_metric(metrics, "state_valid").bool() & _task_metric(metrics, "slot_fault_code").eq(0)
        if self._episode_seconds is None:
            self._episode_seconds = torch.zeros_like(dones, dtype=torch.float64)
            self._episode_linear = torch.zeros_like(dones, dtype=torch.float64)
            self._episode_yaw = torch.zeros_like(dones, dtype=torch.float64)
            self._episode_fault = torch.zeros_like(dones, dtype=torch.bool)
        assert self._episode_linear is not None and self._episode_yaw is not None and self._episode_fault is not None
        actor_command = observations["policy"][:, 9:12]
        speed = _task_metric(metrics, "command_speed")
        linear = _task_metric(metrics, "linear_velocity_error")
        yaw = _task_metric(metrics, "yaw_rate_error")
        # Frozen initial behavior criteria. Each fixed-command case is evaluated separately.
        linear_limit = (speed * 0.5).clamp(min=0.03, max=0.20)
        yaw_limit = (actor_command[:, 2].abs() * 0.5).clamp(min=0.05, max=0.25)
        self._episode_seconds[valid] += dt
        self._episode_linear[valid] += (linear[valid] / linear_limit[valid]).square() * dt
        self._episode_yaw[valid] += (yaw[valid] / yaw_limit[valid]).square() * dt
        self._episode_fault |= ~valid
        self._valid_seconds += int(valid.sum().item()) * dt
        self._linear_squared += float(linear[valid].square().sum().item()) * dt
        self._yaw_squared += float(yaw[valid].square().sum().item()) * dt

        names = (
            "forward_velocity",
            "linear_velocity_error",
            "command_vx",
            "command_speed",
            "action_clip_fraction",
            "torque_clip_fraction",
            "torque_over_limit",
            "action_rate",
            "body_height",
        )
        values = {name: _task_metric(metrics, name) for name in names}
        values.update(
            {
                "actor_command_vx": actor_command[:, 0],
                "actor_command_speed": torch.linalg.vector_norm(actor_command[:, :2], dim=1),
                "command_observation_error": (actor_command[:, 0] - values["command_vx"]).abs(),
            }
        )
        for name, value in values.items():
            self._sums[name] = self._sums.get(name, 0.0) + float(value[valid].sum().item()) * dt
        self._pending_height_dt += dt
        if bool(valid.any()):
            self._body_height_series.append(float(values["body_height"][valid].mean().item()))
            self._body_height_dts.append(self._pending_height_dt)
            self._pending_height_dt = 0.0

        base_contact = _task_metric(metrics, "base_contact").bool()
        timeout = _task_metric(metrics, "timeout").bool()
        fault = completed & self._episode_fault
        fall = completed & ~fault & base_contact
        survived = completed & ~fault & ~fall & timeout
        if bool((completed & ~(fault | fall | survived)).any()):
            raise RuntimeError("PhantomX completed without timeout, fall or Slot fault")
        tracked = (
            survived
            & self._episode_seconds.gt(0)
            & self._episode_linear.lt(self._episode_seconds)
            & self._episode_yaw.lt(self._episode_seconds)
        )
        self._survived += int(survived.sum().item())
        self._tracked += int(tracked.sum().item())
        self._falls += int(fall.sum().item())
        self._faults += int(fault.sum().item())
        age = _task_metric(metrics, "reset_age_steps")
        clearance = _task_metric(metrics, "body_clearance")
        upright = _task_metric(metrics, "upright")
        for slot in torch.where(completed)[0].tolist():
            reason = "slot_fault" if bool(fault[slot]) else "base_contact" if bool(fall[slot]) else "timeout"
            self._termination_events.append(
                {
                    "slot": int(slot),
                    "reset_age_steps": int(age[slot].item()),
                    "ground_clearance": float(clearance[slot].item()),
                    "upright": float(upright[slot].item()),
                    "termination_reason": reason,
                    "tracking_success": bool(tracked[slot]),
                }
            )
        self._episode_seconds[completed] = 0.0
        self._episode_linear[completed] = 0.0
        self._episode_yaw[completed] = 0.0
        self._episode_fault[completed] = False
        reset_events = _task_metric(metrics, "reset_event").bool() & valid
        self._reset_joint_error_sum += float(_task_metric(metrics, "reset_joint_error_max")[reset_events].sum().item())
        self._reset_joint_error_count += int(reset_events.sum().item())

    @override
    def finish(self, summary: EvaluationSummary) -> PhantomXEvaluationResult:
        completed = summary.completed_episodes
        if self._survived + self._falls + self._faults != completed:
            raise ValueError("PhantomX evaluation counts disagree with rollout summary")
        duration = self._valid_seconds

        def mean(name: str) -> float:
            return self._sums.get(name, 0.0) / duration if duration else math.nan

        return PhantomXEvaluationResult(
            task_id=summary.task_id,
            checkpoint=summary.checkpoint,
            steps=summary.steps,
            completed_episodes=completed,
            mean_forward_velocity=mean("forward_velocity"),
            mean_speed_error=mean("linear_velocity_error"),
            survival_rate=self._survived / completed if completed else 0.0,
            tracking_success_rate=self._tracked / completed if completed else 0.0,
            linear_velocity_rmse=math.sqrt(self._linear_squared / duration) if duration else math.nan,
            yaw_rate_rmse=math.sqrt(self._yaw_squared / duration) if duration else math.nan,
            fault_rate=self._faults / completed if completed else 0.0,
            fall_rate=self._falls / completed if completed else 0.0,
            base_contact_rate=self._falls / completed if completed else 0.0,
            mean_episode_length=summary.mean_episode_length,
            mean_command_vx=mean("command_vx"),
            mean_command_speed=mean("command_speed"),
            mean_actor_command_vx=mean("actor_command_vx"),
            mean_actor_command_speed=mean("actor_command_speed"),
            mean_command_observation_error=mean("command_observation_error"),
            mean_action_clip_fraction=mean("action_clip_fraction"),
            mean_torque_clip_fraction=mean("torque_clip_fraction"),
            mean_torque_over_limit=mean("torque_over_limit"),
            mean_action_rate=mean("action_rate"),
            mean_body_height=mean("body_height"),
            mean_reset_joint_error=(
                self._reset_joint_error_sum / self._reset_joint_error_count if self._reset_joint_error_count else 0.0
            ),
            dominant_body_height_frequency_hz=_dominant_frequency_hz(
                self._body_height_series,
                sample_hz=self._sample_hz,
                sample_dts=self._body_height_dts,
            ),
            termination_events=tuple(self._termination_events),
        )


def format_phantomx_evaluation(result: object) -> str:
    """Format the established PhantomX verification line."""

    report = cast(PhantomXEvaluationResult, result)
    return (
        f"[VERIFY] task={report.task_id} checkpoint={report.checkpoint} steps={report.steps} "
        f"episodes={report.completed_episodes} forward_velocity={report.mean_forward_velocity:.6f} "
        f"speed_error={report.mean_speed_error:.6f} survival_rate={report.survival_rate:.6f} "
        f"tracking_success_rate={report.tracking_success_rate:.6f} "
        f"linear_rmse={report.linear_velocity_rmse:.6f} yaw_rmse={report.yaw_rate_rmse:.6f} "
        f"fault_rate={report.fault_rate:.6f} fall_rate={report.fall_rate:.6f} "
        f"base_contact_rate={report.base_contact_rate:.6f} "
        f"episode_length={report.mean_episode_length:.3f} "
        f"command_vx={report.mean_command_vx:.6f} command_speed={report.mean_command_speed:.6f} "
        f"actor_command_vx={report.mean_actor_command_vx:.6f} "
        f"actor_command_speed={report.mean_actor_command_speed:.6f} "
        f"command_obs_error={report.mean_command_observation_error:.6f} "
        f"action_clip_fraction={report.mean_action_clip_fraction:.6f} "
        f"torque_clip_fraction={report.mean_torque_clip_fraction:.6f} "
        f"torque_over_limit={report.mean_torque_over_limit:.6f} "
        f"action_rate={report.mean_action_rate:.6f} mean_body_height={report.mean_body_height:.6f} "
        f"body_height_frequency_hz={report.dominant_body_height_frequency_hz:.6f} "
        f"reset_joint_error={report.mean_reset_joint_error:.6f} shutdown=PASS"
    )


def _task_metric(metrics: Mapping[str, object], name: str) -> torch.Tensor:
    """Select one namespaced PhantomX metric by its stable leaf name."""

    value = next(value for key, value in metrics.items() if key.rsplit("/", 1)[-1] == name)
    return value if isinstance(value, torch.Tensor) else torch.as_tensor(value)


def _dominant_frequency_hz(
    values: Sequence[float],
    *,
    sample_hz: float,
    sample_dts: Sequence[float] | None = None,
) -> float:
    """Return the strongest non-drift frequency in a sampled body-height signal."""

    if len(values) < 4:
        return 0.0
    signal = torch.tensor(values, dtype=torch.float64)
    if sample_dts is not None:
        durations = torch.tensor(sample_dts, dtype=torch.float64)
        if durations.numel() != signal.numel() or bool((durations <= 0).any()):
            raise ValueError("sample_dts must contain one positive interval per sample")
        if bool(torch.allclose(durations, durations[:1], rtol=1e-9, atol=1e-12)):
            sample_hz = 1.0 / float(durations[0].item())
        else:
            times = durations.cumsum(0)
            span = float((times[-1] - times[0]).item())
            max_frequency = 0.5 / float(durations.max().item())
            frequencies = torch.arange(0.5, max_frequency, 1.0 / (4.0 * span))
            centered = signal - (signal * durations).sum() / durations.sum()
            angles = 2.0 * torch.pi * frequencies[:, None] * times[None, :]
            weighted = centered * durations
            power = (torch.cos(angles) @ weighted).square() + (torch.sin(angles) @ weighted).square()
            if power.numel() == 0 or float(power.max().item()) <= 1.0e-12:
                return 0.0
            return float(frequencies[int(torch.argmax(power).item())].item())
    signal = signal - signal.mean()
    power = torch.fft.rfft(signal).abs().square()
    frequencies = torch.fft.rfftfreq(signal.numel(), d=1.0 / sample_hz)
    eligible = frequencies >= 0.5
    if not bool(eligible.any()) or float(power[eligible].max().item()) <= 1.0e-12:
        return 0.0
    eligible_indices = torch.where(eligible)[0]
    peak = eligible_indices[int(torch.argmax(power[eligible]).item())]
    return float(frequencies[peak].item())


__all__ = ["PhantomXEvaluationResult", "PhantomXEvaluator", "format_phantomx_evaluation"]

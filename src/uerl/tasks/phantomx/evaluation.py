"""PhantomX-owned evaluation aggregation and CLI formatting."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import cast, final

import torch
from typing_extensions import override

from ..evaluation import EvaluationSummary, TaskEvaluator


@dataclass(frozen=True, slots=True)
class PhantomXEvaluationResult:
    """Report time-weighted tracking errors and episode survival separately."""

    task_id: str
    checkpoint: Path
    steps: int
    completed_episodes: int
    mean_forward_velocity: float
    mean_speed_error: float
    survival_rate: float
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
    measured_seconds: float = 0.0
    linear_velocity_rmse: float = 0.0
    yaw_rate_rmse: float = 0.0
    termination_events: tuple[Mapping[str, object], ...] = ()


@final
class PhantomXEvaluator(TaskEvaluator):
    """Aggregate only the metrics and observation semantics owned by PhantomX."""

    def __init__(self, sample_hz: float) -> None:
        self._sample_hz = sample_hz
        self._forward_velocity_sum = 0.0
        self._speed_error_sum = 0.0
        self._command_vx_sum = 0.0
        self._command_speed_sum = 0.0
        self._actor_command_vx_sum = 0.0
        self._actor_command_speed_sum = 0.0
        self._command_observation_error_sum = 0.0
        self._action_clip_fraction_sum = 0.0
        self._torque_clip_fraction_sum = 0.0
        self._torque_over_limit_sum = 0.0
        self._reset_joint_error_sum = 0.0
        self._reset_joint_error_count = 0
        self._action_rate_sum = 0.0
        self._body_height_sum = 0.0
        self._body_height_series: list[float] = []
        self._body_height_dts: list[float] = []
        self._survived_episodes = 0
        self._fall_episodes = 0
        self._base_contact_episodes = 0
        self._termination_events: list[Mapping[str, object]] = []
        self._measured_seconds = 0.0
        self._linear_error_squared_integral = 0.0
        self._yaw_error_squared_integral = 0.0

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
        dt = float(cast(float, metrics.get("transition_dt_s", 1.0 / self._sample_hz)))
        self._measured_seconds += dt
        self._linear_error_squared_integral += float(
            _task_metric(metrics, "linear_velocity_error").square().mean().item()
        ) * dt
        self._yaw_error_squared_integral += float(
            _task_metric(metrics, "yaw_rate_error").square().mean().item()
        ) * dt
        actor_command = observations["policy"][:, 9:12]
        command_vx = _task_metric(metrics, "command_vx")
        self._actor_command_vx_sum += float(actor_command[:, 0].mean().item()) * dt
        self._actor_command_speed_sum += float(
            torch.linalg.vector_norm(actor_command[:, :2], dim=1).mean().item()
        ) * dt
        self._forward_velocity_sum += float(_task_metric(metrics, "forward_velocity").mean().item()) * dt
        self._speed_error_sum += float(_task_metric(metrics, "linear_velocity_error").mean().item()) * dt
        command_speed = _task_metric(metrics, "command_speed")
        self._command_vx_sum += float(command_vx.mean().item()) * dt
        self._command_speed_sum += float(command_speed.mean().item()) * dt
        self._command_observation_error_sum += float(
            (actor_command[:, 0] - command_vx).abs().mean().item()
        ) * dt
        self._action_clip_fraction_sum += float(_task_metric(metrics, "action_clip_fraction").mean().item()) * dt
        self._torque_clip_fraction_sum += float(_task_metric(metrics, "torque_clip_fraction").mean().item()) * dt
        self._torque_over_limit_sum += float(_task_metric(metrics, "torque_over_limit").mean().item()) * dt
        action_rate = _task_metric(metrics, "action_rate")
        body_height = _task_metric(metrics, "body_height")
        self._action_rate_sum += float(action_rate.mean().item()) * dt
        self._body_height_sum += float(body_height.mean().item()) * dt
        self._body_height_series.append(float(body_height.mean().item()))
        self._body_height_dts.append(dt)
        completed = dones.bool()
        base_contact = _task_metric(metrics, "base_contact").bool()
        timeout = _task_metric(metrics, "timeout").bool()
        self._survived_episodes += int((completed & timeout & ~base_contact).sum().item())
        reset_age_steps = _task_metric(metrics, "reset_age_steps")
        ground_clearance = _task_metric(metrics, "body_clearance")
        upright = _task_metric(metrics, "upright")
        for slot_id in torch.where(completed)[0].tolist():
            if bool(base_contact[slot_id].item()):
                reason = "base_contact"
            elif bool(timeout[slot_id].item()):
                reason = "timeout"
            else:
                raise RuntimeError(
                    "PhantomX evaluation received a completed Slot without a Task termination reason"
                )
            self._termination_events.append(
                {
                    "slot": int(slot_id),
                    "reset_age_steps": int(reset_age_steps[slot_id].item()),
                    "ground_clearance": float(ground_clearance[slot_id].item()),
                    "upright": float(upright[slot_id].item()),
                    "termination_reason": reason,
                }
            )
        reset_events = _task_metric(metrics, "reset_event").bool()
        self._reset_joint_error_sum += float(
            _task_metric(metrics, "reset_joint_error_max")[reset_events].sum().item()
        )
        self._reset_joint_error_count += int(reset_events.sum().item())
        self._fall_episodes += int(base_contact.sum().item())
        self._base_contact_episodes += int(base_contact.sum().item())

    @override
    def finish(self, summary: EvaluationSummary) -> PhantomXEvaluationResult:
        completed = summary.completed_episodes
        seconds = self._measured_seconds
        return PhantomXEvaluationResult(
            task_id=summary.task_id,
            checkpoint=summary.checkpoint,
            steps=summary.steps,
            completed_episodes=completed,
            mean_forward_velocity=self._forward_velocity_sum / seconds,
            mean_speed_error=self._speed_error_sum / seconds,
            survival_rate=self._survived_episodes / completed if completed else 0.0,
            fall_rate=self._fall_episodes / completed if completed else 0.0,
            base_contact_rate=self._base_contact_episodes / completed if completed else 0.0,
            mean_episode_length=summary.mean_episode_length,
            mean_command_vx=self._command_vx_sum / seconds,
            mean_command_speed=self._command_speed_sum / seconds,
            mean_actor_command_vx=self._actor_command_vx_sum / seconds,
            mean_actor_command_speed=self._actor_command_speed_sum / seconds,
            mean_command_observation_error=self._command_observation_error_sum / seconds,
            mean_action_clip_fraction=self._action_clip_fraction_sum / seconds,
            mean_torque_clip_fraction=self._torque_clip_fraction_sum / seconds,
            mean_torque_over_limit=self._torque_over_limit_sum / seconds,
            mean_reset_joint_error=(
                self._reset_joint_error_sum / self._reset_joint_error_count
                if self._reset_joint_error_count
                else 0.0
            ),
            mean_action_rate=self._action_rate_sum / seconds,
            mean_body_height=self._body_height_sum / seconds,
            dominant_body_height_frequency_hz=_dominant_frequency_hz(
                self._body_height_series,
                sample_hz=self._sample_hz,
                sample_dts=self._body_height_dts,
            ),
            measured_seconds=seconds,
            linear_velocity_rmse=(self._linear_error_squared_integral / seconds) ** 0.5,
            yaw_rate_rmse=(self._yaw_error_squared_integral / seconds) ** 0.5,
            termination_events=tuple(self._termination_events),
        )


def format_phantomx_evaluation(result: object) -> str:
    """Format the established PhantomX verification line."""

    report = cast(PhantomXEvaluationResult, result)
    return (
        f"[VERIFY] task={report.task_id} checkpoint={report.checkpoint} steps={report.steps} "
        f"episodes={report.completed_episodes} forward_velocity={report.mean_forward_velocity:.6f} "
        f"speed_error={report.mean_speed_error:.6f} survival_rate={report.survival_rate:.6f} "
        f"linear_velocity_rmse={report.linear_velocity_rmse:.6f} "
        f"yaw_rate_rmse={report.yaw_rate_rmse:.6f} measured_seconds={report.measured_seconds:.6f} "
        f"fall_rate={report.fall_rate:.6f} "
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

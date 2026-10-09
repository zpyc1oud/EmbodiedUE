"""Compute read-only learning diagnostics from single-Slot Task traces."""

from __future__ import annotations

import math
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from ..core.config.yaml_loader import load_unique_yaml


def _mapping(value: object, path: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a mapping")
    return value


def _number(value: object, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{path}: expected a finite number")
    return float(value)


def _vector(value: object, width: int | None, path: str) -> list[float]:
    if not isinstance(value, list) or not value or (width is not None and len(value) != width):
        raise ValueError(f"{path}: expected {'a nonempty' if width is None else width}-value list")
    return [_number(item, f"{path}[{index}]") for index, item in enumerate(value)]


def _body_velocity(velocity: list[float], pose: list[float], path: str) -> list[float]:
    # Inverse quaternion rotation: v + 2 * (w * (q x v) + q x (q x v)).
    x, y, z, w = pose[3:]
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if abs(norm - 1.0) > 1e-3:
        raise ValueError(f"{path}: expected a unit quaternion")
    x, y, z, w = -x / norm, -y / norm, -z / norm, w / norm
    vx, vy, vz = velocity
    a, b, c = y * vz - z * vy, z * vx - x * vz, x * vy - y * vx
    return [vx + 2 * (w * a + y * c - z * b), vy + 2 * (w * b + z * a - x * c), vz + 2 * (w * c + x * b - y * a)]


def diagnose_task_trace(
    path: Path,
    *,
    body: str = "base_link",
    command: str = "velocity",
    low_motion_speed: float = 0.01,
) -> dict[str, object]:
    """Report recorded intervals; do not infer missing force or success evidence."""
    if not math.isfinite(low_motion_speed) or low_motion_speed < 0:
        raise ValueError("low_motion_speed must be finite and nonnegative")
    trace = _mapping(load_unique_yaml(path.read_text(encoding="utf-8")), "trace")
    if trace.get("schema_version") != 1:
        raise ValueError("schema_version: expected Task trace version 1")
    source = _mapping(trace.get("source"), "source")
    if source.get("side") != "task":
        raise ValueError("source.side: expected task")
    records = trace.get("records")
    if not isinstance(records, list):
        raise ValueError("records: expected a list")
    duration = linear_squared = yaw_squared = path_length = reward_sum = 0.0
    action_squared = action_delta_squared = moving_time = moving_path = 0.0
    action_count = action_delta_count = steps = terminated = truncated = 0
    previous_action: list[float] | None = None
    previous_key: tuple[int, int] | None = None
    episodes: dict[int, list[float]] = {}
    forces: dict[str, list[float]] = {}
    prefix = f"robot.body.{body}."
    for index, value in enumerate(records):
        row = _mapping(value, f"records[{index}]")
        if row.get("kind") != "step":
            continue
        label = f"records[{index}]"
        episode, step = row.get("episode_index"), row.get("episode_step")
        if type(episode) is not int or episode < 0 or type(step) is not int or step < 0:
            raise ValueError(f"{label}: invalid episode_index or episode_step")
        if previous_key is not None and (episode, step) <= previous_key:
            raise ValueError(f"{label}: episode/step order must increase")
        clocks = _mapping(row.get("clocks"), f"{label}.clocks")
        dt = _number(clocks.get("transition_dt_s"), f"{label}.clocks.transition_dt_s")
        if dt <= 0:
            raise ValueError(f"{label}.clocks.transition_dt_s: must be positive")
        before = _mapping(row.get("input"), f"{label}.input")
        after = _mapping(row.get("transition"), f"{label}.transition")
        for name, state in (("input", before), ("transition", after)):
            if state.get("state_valid") is not True or state.get("fault_code") != 0:
                raise ValueError(f"{label}.{name}: invalid or faulted state")
        initial = _mapping(before.get("raw_state"), f"{label}.input.raw_state")
        final = _mapping(after.get("raw_state"), f"{label}.transition.raw_state")
        start = _vector(initial.get(prefix + "body_pose"), 7, f"{label}.input.{prefix}body_pose")
        pose = _vector(final.get(prefix + "body_pose"), 7, f"{label}.transition.{prefix}body_pose")
        velocity = _vector(final.get(prefix + "body_linear_velocity"), 3, f"{label}.{prefix}body_linear_velocity")
        angular = _vector(final.get(prefix + "body_angular_velocity"), 3, f"{label}.{prefix}body_angular_velocity")
        linear_body = _body_velocity(velocity, pose, label)
        angular_body = _body_velocity(angular, pose, label)
        commands = _mapping(before.get("commands"), f"{label}.input.commands")
        target = _vector(commands.get(command), 3, f"{label}.input.commands.{command}")
        linear_squared += ((linear_body[0] - target[0]) ** 2 + (linear_body[1] - target[1]) ** 2) * dt
        yaw_squared += (angular_body[2] - target[2]) ** 2 * dt
        dx, dy = pose[0] - start[0], pose[1] - start[1]
        distance = math.hypot(dx, dy)
        path_length += distance
        displacement = episodes.setdefault(episode, [0.0, 0.0])
        displacement[0] += dx
        displacement[1] += dy
        if math.hypot(*target[:2]) > low_motion_speed:
            moving_time += dt
            moving_path += distance
        action = _mapping(row.get("action"), f"{label}.action")
        values = _vector(action.get("policy_action"), None, f"{label}.action.policy_action")
        action_squared += sum(item * item for item in values)
        action_count += len(values)
        if previous_action is not None and previous_key == (episode, step - 1):
            if len(previous_action) != len(values):
                raise ValueError(f"{label}.action: action width changed")
            action_delta_squared += sum((a - b) ** 2 for a, b in zip(values, previous_action, strict=True))
            action_delta_count += len(values)
        previous_action, previous_key = values, (episode, step)
        for field, field_value in final.items():
            if field.endswith(".contact_force"):
                force = _vector(field_value, 1, f"{label}.{field}")[0]
                if force < 0:
                    raise ValueError(f"{label}.{field}: contact force magnitude must be nonnegative")
                stats = forces.setdefault(field, [0.0, 0.0, 0.0])
                stats[0] += 1
                stats[1] += float(force > 0)
                stats[2] = max(stats[2], force)
        reward_sum += _number(after.get("reward"), f"{label}.transition.reward")
        for name in ("terminated", "truncated"):
            if type(after.get(name)) is not bool:
                raise ValueError(f"{label}.transition.{name}: expected boolean")
        terminated += int(after["terminated"])
        truncated += int(after["truncated"] and not after["terminated"])
        duration += dt
        steps += 1
    if not steps:
        raise ValueError("records: no step records")
    findings: list[str] = []
    if moving_time and moving_path / moving_time < low_motion_speed:
        findings.append("Low measured motion during nonzero planar commands; inspect actions and physical response.")
    if forces and all(stats[1] == 0 for stats in forces.values()):
        findings.append("All recorded force samples are zero; verify contact and the force observation path.")
    return {
        "source": dict(source),
        "trace_status": trace.get("status"),
        "steps": steps,
        "measured_seconds": duration,
        "completed_episodes": terminated + truncated,
        "terminated_episodes": terminated,
        "timeout_only_episodes": truncated,
        "linear_velocity_rmse_m_s": math.sqrt(linear_squared / duration),
        "yaw_rate_rmse_rad_s": math.sqrt(yaw_squared / duration),
        "planar_path_length_m": path_length,
        "episode_measured_displacement_m": {key: value for key, value in episodes.items()},
        "action_rms": math.sqrt(action_squared / action_count),
        "within_episode_action_delta_rms": (
            math.sqrt(action_delta_squared / action_delta_count) if action_delta_count else None
        ),
        "reward_sum": reward_sum,
        "reward_components": None,
        "success_rate": None,
        "contact_force": {
            key: {"samples": int(stats[0]), "positive_samples": int(stats[1]), "maximum_n": stats[2]}
            for key, stats in forces.items()
        }
        or None,
        "low_motion_speed_threshold_m_s": low_motion_speed,
        "findings": findings,
        "limitations": [
            "Single recorded Slot only. Success and reward components are not present in this trace.",
            "Displacement sums measured intervals only; reset jumps are excluded.",
        ],
    }


def summarize_training_scalars(directory: Path, *, last_iterations: int = 100) -> dict[str, object]:
    """Summarize recorded TensorBoard scalars without mixing them with rollout metrics."""
    from tensorboard.backend.event_processing.event_accumulator import EventAccumulator

    if last_iterations <= 0:
        raise ValueError("last_iterations must be positive")
    if not directory.is_dir():
        raise ValueError(f"events: directory does not exist: {directory}")
    events = EventAccumulator(str(directory), size_guidance={"scalars": 0}).Reload()
    tags = events.Tags().get("scalars", [])
    if not tags:
        raise ValueError(f"events: no scalar series in {directory}")
    report: dict[str, object] = {}
    for tag in sorted(tags):
        # Continuation logs can contain the same step twice. Keep the last written value.
        by_step = {sample.step: sample.value for sample in events.Scalars(tag)}
        last_step = max(by_step)
        selected = sorted((step, value) for step, value in by_step.items() if step > last_step - last_iterations)
        values = [_number(value, f"events.{tag}[{step}]") for step, value in selected]
        report[tag] = {
            "latest_step": last_step,
            "latest": by_step[last_step],
            "window_first_step": selected[0][0],
            "window_samples": len(values),
            "window_mean": sum(values) / len(values),
        }
    return {
        "directory": str(directory),
        "last_iterations": last_iterations,
        "scalars": report,
        "meaning": "Means of logged iteration scalars. Episode/reward terms retain their writer's aggregation; "
        "they are not rollout RMSE or a new sum of episode rewards.",
    }

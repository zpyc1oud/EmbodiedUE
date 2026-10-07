"""Task-side YAML traces and conservative Task/UE policy-frame comparisons."""

from __future__ import annotations

import math
import re
import struct
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any, cast

import yaml

from ..core.config.yaml_loader import load_unique_yaml
from ..core.direct.robot_action import ROBOT_ACTUATOR_TARGET_FIELD

_TRACE_SCHEMA_VERSION = 1
_ONNX_SHA1_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class TaskPolicyTraceRecorder:
    """Collect one-Slot DirectEnv rows and write a project YAML trace on finish."""

    def __init__(
        self,
        path: Path,
        *,
        source: Mapping[str, object],
        clock: Mapping[str, object],
        actor_observation_groups: Sequence[str],
    ) -> None:
        self.path = Path(path)
        if self.path.exists():
            raise FileExistsError(f"trace already exists: {self.path}")
        self.source = cast(dict[str, object], _yaml_value(source))
        self.clock = cast(dict[str, object], _yaml_value(clock))
        self.actor_observation_groups = tuple(actor_observation_groups)
        self.deployment_state_fields: tuple[str, ...] = ()
        self.records: list[dict[str, object]] = []
        self._finished = False

    def record_step(self, step: Mapping[str, object]) -> None:
        """Store a decision input and its separate pre-reset transition result."""

        if self._finished:
            raise RuntimeError("cannot record a step after the trace is finished")
        record = cast(dict[str, object], _yaml_value(step))
        input_row = cast(dict[str, object], record["input"])
        groups = cast(Mapping[str, Sequence[float | int | bool]], input_row["observation_groups"])
        absent = [name for name in self.actor_observation_groups if name not in groups]
        if absent:
            raise ValueError(f"Task trace is missing actor observation groups: {absent}")
        input_row["observation"] = [
            float(value)
            for group in self.actor_observation_groups
            for value in groups[group]
        ]
        record["kind"] = "step"
        record["sequence"] = len(self.records) + 1
        self.records.append(record)

    def set_policy_fingerprint(self, onnx_sha1: str) -> None:
        """Attach the exported ONNX identity before the first policy step."""

        if self._finished or self.records:
            raise RuntimeError("policy fingerprint must be set before recording steps")
        if not _ONNX_SHA1_PATTERN.fullmatch(onnx_sha1):
            raise ValueError("policy ONNX fingerprint must be a 40-character lowercase SHA-1")
        self.source["policy_onnx_sha1"] = onnx_sha1

    def set_deployment_state_fields(self, fields: Sequence[str]) -> None:
        """Record the exact state fields selected by the bound deployment plan."""

        if self._finished or self.records:
            raise RuntimeError("deployment state fields must be set before recording steps")
        normalized = tuple(fields)
        if not normalized or any(not isinstance(name, str) or not name for name in normalized):
            raise ValueError("deployment state fields must be a non-empty sequence of names")
        if len(set(normalized)) != len(normalized):
            raise ValueError("deployment state fields must be unique")
        self.deployment_state_fields = normalized

    def finish(self, *, complete: bool) -> Path:
        """Persist the trace and say whether the requested evaluation finished."""

        if self._finished:
            return self.path
        field_layout = _task_field_layout(
            self.records,
            self.actor_observation_groups,
            self.deployment_state_fields,
        )
        payload = {
            "schema_version": _TRACE_SCHEMA_VERSION,
            "source": {"side": "task", **self.source},
            "clock": self.clock,
            "actor_observation_groups": list(self.actor_observation_groups),
            "field_layout": field_layout,
            "status": "complete" if complete else "incomplete",
            "records": self.records,
        }
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("x", encoding="utf-8") as trace_file:
            trace_file.write(yaml.safe_dump(payload, sort_keys=False, allow_unicode=True))
        self._finished = True
        return self.path


def compare_policy_traces(task_path: Path, ue_path: Path) -> dict[str, object]:
    """Compare phase-aligned Task and UE decision rows without a quality threshold."""

    task = _read_trace(task_path, expected_side="task")
    ue = _read_trace(ue_path, expected_side="ue")
    global_reasons = (
        _identity_mismatches(task, ue)
        + _clock_mismatches(task, ue)
        + _layout_mismatches(task, ue)
    )
    deployment_state_fields = _required_state_fields(task)
    task_rows = _index_records(task, "step")
    ue_rows = _index_records(ue, "frame")
    ue_events = _index_events(ue)
    keys = sorted(set(task_rows) | set(ue_rows) | set(ue_events))
    ue_faulted = ue.get("status") == "faulted" or any(
        event.get("event") == "policy_fault"
        for events in ue_events.values()
        for event in events
    )
    task_faulted = any(_task_row_events(row) for row in task_rows.values())
    pairs: list[dict[str, object]] = []
    for key in keys:
        task_row = task_rows.get(key)
        ue_row = ue_rows.get(key)
        frame_events = ue_events.get(key, [])
        reasons = list(global_reasons)
        if task_row is None:
            reasons.append("Task row is missing for this episode/policy-step")
        if ue_row is None:
            reasons.append("UE row is missing for this episode/policy-step")
        if task_row is not None and ue_row is not None:
            reasons.extend(
                _row_alignment_mismatches(task_row, ue_row, deployment_state_fields)
            )
        if any(event.get("event") == "policy_fault" for event in frame_events):
            reasons.append("UE policy fault occurred before this policy decision frame")
        task_events = [] if task_row is None else _task_row_events(task_row)
        pair: dict[str, object] = {
            "episode_index": key[0],
            "episode_step": key[1],
            "task_sequence": None if task_row is None else task_row.get("sequence"),
            "ue_sequence": None if ue_row is None else ue_row.get("sequence"),
            "comparable": not reasons,
            "reasons": reasons,
            "events": {"task": task_events, "ue": frame_events},
        }
        if task_row is not None:
            task_input_value = task_row.get("input")
            task_state_value = (
                task_input_value.get("raw_state")
                if isinstance(task_input_value, Mapping)
                else None
            )
            task_fields = set(task_state_value) if isinstance(task_state_value, Mapping) else set()
            pair["task_diagnostic_only_state_fields"] = sorted(
                task_fields - set(deployment_state_fields)
            )
        if not reasons and task_row is not None and ue_row is not None:
            pair["actual_time_s"] = {
                "task_episode": _nested(task_row, "clocks", "input_episode_elapsed_s"),
                "ue_episode": _nested(ue_row, "clocks", "episode_elapsed_s"),
                "ue_solver": _nested(ue_row, "clocks", "solver_time_s"),
            }
            pair["metrics"] = _row_metrics(task_row, ue_row, deployment_state_fields)
            pair["successor"] = _successor_comparison(
                task_row, ue_rows, deployment_state_fields
            )
        pairs.append(pair)

    comparable_count = sum(bool(pair["comparable"]) for pair in pairs)
    faulted_sources = [
        source
        for source, faulted in (("task", task_faulted), ("ue", ue_faulted))
        if faulted
    ]
    if faulted_sources:
        status = "faulted"
    elif comparable_count == 0:
        status = "not_comparable"
    else:
        status = "comparable"
    return {
        "status": status,
        "faulted_sources": faulted_sources,
        "comparison": "episode_index + policy_step candidate; phase/command/clock checked",
        "task_source": task.get("source"),
        "ue_source": ue.get("source"),
        "trace_statuses": {"task": task.get("status"), "ue": ue.get("status")},
        "task_events": _records_of_kind(task, "event"),
        "ue_events": _records_of_kind(ue, "event"),
        "pair_count": len(pairs),
        "comparable_pair_count": comparable_count,
        "pairs": pairs,
    }


def _read_trace(path: Path, *, expected_side: str) -> dict[str, Any]:
    try:
        loaded = load_unique_yaml(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, yaml.YAMLError, TypeError, ValueError) as exc:
        raise ValueError(f"could not read {expected_side} trace {path}: {exc}") from exc
    if not isinstance(loaded, Mapping):
        raise ValueError(f"{expected_side} trace root must be a mapping")
    trace = cast(dict[str, Any], loaded)
    if trace.get("schema_version") != _TRACE_SCHEMA_VERSION:
        raise ValueError(f"unsupported trace schema in {path}: {trace.get('schema_version')!r}")
    source = trace.get("source")
    if not isinstance(source, Mapping) or source.get("side") != expected_side:
        raise ValueError(f"{path} must identify source.side as {expected_side!r}")
    status = trace.get("status")
    if status not in ({"complete", "incomplete", "faulted"} if expected_side == "ue" else {"complete", "incomplete"}):
        raise ValueError(f"{expected_side} trace has an invalid status {status!r}: {path}")
    if status == "incomplete":
        raise ValueError(f"{expected_side} trace is incomplete: {path}")
    records = trace.get("records")
    if not isinstance(records, list):
        raise ValueError(f"{expected_side} trace records must be a list")
    expected_kind = "step" if expected_side == "task" else "frame"
    for index, record in enumerate(records):
        if not isinstance(record, Mapping):
            raise ValueError(f"{expected_side} trace record {index} must be a mapping")
        if record.get("kind") == expected_kind:
            for key in ("episode_index", "episode_step", "phase", "input", "action", "clocks"):
                if key not in record:
                    raise ValueError(f"{expected_side} trace record {index} is missing {key!r}")
    return trace


def _identity_mismatches(task: Mapping[str, Any], ue: Mapping[str, Any]) -> list[str]:
    task_source = cast(Mapping[str, Any], task["source"])
    ue_source = cast(Mapping[str, Any], ue["source"])
    reasons: list[str] = []
    for field in ("task_id", "robot_id", "map_package", "seed"):
        task_value = task_source.get(field)
        ue_value = ue_source.get(field)
        if task_value is None or ue_value is None:
            reasons.append(f"{field} identity is missing (Task={task_value!r}, UE={ue_value!r})")
        elif task_value != ue_value:
            reasons.append(f"{field} differs (Task={task_value!r}, UE={ue_value!r})")
    task_fingerprint = task_source.get("policy_onnx_sha1")
    ue_fingerprint = ue_source.get("policy_onnx_sha1")
    if task_fingerprint is None or ue_fingerprint is None:
        reasons.append(
            "policy ONNX fingerprint is missing "
            f"(Task={task_fingerprint!r}, UE={ue_fingerprint!r})"
        )
    elif (
        not isinstance(task_fingerprint, str)
        or not isinstance(ue_fingerprint, str)
        or not _ONNX_SHA1_PATTERN.fullmatch(task_fingerprint)
        or not _ONNX_SHA1_PATTERN.fullmatch(ue_fingerprint)
    ):
        reasons.append("policy ONNX fingerprint is malformed")
    elif task_fingerprint != ue_fingerprint:
        reasons.append(
            f"policy ONNX fingerprint differs "
            f"(Task={task_fingerprint!r}, UE={ue_fingerprint!r})"
        )
    return reasons


def _clock_mismatches(task: Mapping[str, Any], ue: Mapping[str, Any]) -> list[str]:
    task_clock = task.get("clock")
    ue_clock = ue.get("clock")
    if not isinstance(task_clock, Mapping) or not isinstance(ue_clock, Mapping):
        return ["clock metadata is missing"]
    reasons: list[str] = []
    task_dt = task_clock.get("physics_dt_s")
    ue_dt = ue_clock.get("physics_dt_s")
    if task_dt is None or ue_dt is None or not _same_clock(task_dt, ue_dt):
        reasons.append(f"physics dt differs (Task={task_dt!r}, UE={ue_dt!r})")
    task_decimation = task_clock.get("decimation_range")
    ue_decimation = (ue_clock.get("decimation_min"), ue_clock.get("decimation_max"))
    if not isinstance(task_decimation, Sequence) or len(task_decimation) != 2:
        reasons.append(f"Task decimation range is missing or invalid ({task_decimation!r})")
    elif None in ue_decimation or list(task_decimation) != list(ue_decimation):
        reasons.append(
            f"decimation range differs (Task={list(task_decimation)!r}, UE={list(ue_decimation)!r})"
        )
    return reasons


def _layout_mismatches(task: Mapping[str, Any], ue: Mapping[str, Any]) -> list[str]:
    task_layout = task.get("field_layout")
    ue_layout = ue.get("field_layout")
    if not isinstance(task_layout, Mapping) or not isinstance(ue_layout, Mapping):
        return ["field layout is missing"]
    reasons: list[str] = []
    task_state_fields = _layout_state_fields(task_layout, "Task", reasons)
    ue_state_fields = _layout_state_fields(ue_layout, "UE", reasons)
    required_value = task_layout.get("deployment_state_fields")
    if (
        not isinstance(required_value, Sequence)
        or isinstance(required_value, str | bytes)
        or not required_value
        or any(not isinstance(name, str) or not name for name in required_value)
    ):
        reasons.append("Task deployment state field requirements are missing or invalid")
        required_fields: tuple[str, ...] = ()
    else:
        required_fields = tuple(cast(Sequence[str], required_value))
        if len(set(required_fields)) != len(required_fields):
            reasons.append("Task deployment state field requirements contain duplicates")

    for name in required_fields:
        task_width = task_state_fields.get(name)
        ue_width = ue_state_fields.get(name)
        if task_width is None:
            reasons.append(f"deployment-required raw state field {name!r} is missing from Task layout")
        if ue_width is None:
            reasons.append(f"deployment-required raw state field {name!r} is missing from UE layout")
        elif task_width is not None and task_width != ue_width:
            reasons.append(
                f"deployment-required raw state field {name!r} width differs "
                f"(Task={task_width}, UE={ue_width})"
            )
    unexpected_ue_fields = sorted(set(ue_state_fields) - set(required_fields))
    if unexpected_ue_fields:
        reasons.append(
            "UE raw state fields do not match the deployment plan; "
            f"unexpected fields={unexpected_ue_fields}"
        )
    for task_field, ue_field, label in (
        ("actor_observation_width", "observation_width", "observation"),
        ("previous_action_width", "previous_action_width", "previous_action"),
        ("action_width", "action_width", "action"),
        ("actuator_target_width", "actuator_target_width", "actuator targets"),
    ):
        left = task_layout.get(task_field)
        right = ue_layout.get(ue_field)
        if left is None or right is None:
            reasons.append(f"{label} width is missing (Task={left!r}, UE={right!r})")
        elif left != right:
            reasons.append(f"{label} width differs (Task={left!r}, UE={right!r})")
    return reasons


def _layout_state_fields(
    layout: Mapping[str, Any], side: str, reasons: list[str]
) -> dict[str, int]:
    raw_fields = layout.get("raw_state_fields")
    if not isinstance(raw_fields, Sequence) or isinstance(raw_fields, str | bytes):
        reasons.append(f"{side} raw state field layout is missing or invalid")
        return {}
    fields: dict[str, int] = {}
    for index, item in enumerate(raw_fields):
        if not isinstance(item, Mapping):
            reasons.append(f"{side} raw state field layout entry {index} is invalid")
            continue
        name = item.get("name")
        width = item.get("width")
        if not isinstance(name, str) or not name:
            reasons.append(f"{side} raw state field layout entry {index} has an invalid name")
            continue
        if not isinstance(width, int) or isinstance(width, bool) or width < 1:
            reasons.append(f"{side} raw state field {name!r} has an invalid width {width!r}")
            continue
        if name in fields:
            reasons.append(f"{side} raw state field layout repeats {name!r}")
            continue
        fields[name] = width
    return fields


def _index_records(trace: Mapping[str, Any], kind: str) -> dict[tuple[int, int], Mapping[str, Any]]:
    indexed: dict[tuple[int, int], Mapping[str, Any]] = {}
    for record in cast(Sequence[Mapping[str, Any]], trace["records"]):
        if record.get("kind") != kind:
            continue
        key = (int(record["episode_index"]), int(record["episode_step"]))
        if key in indexed:
            raise ValueError(f"duplicate {kind} record for episode/policy-step {key}")
        indexed[key] = record
    return indexed


def _records_of_kind(trace: Mapping[str, Any], kind: str) -> list[Mapping[str, Any]]:
    return [
        record
        for record in cast(Sequence[Mapping[str, Any]], trace["records"])
        if record.get("kind") == kind
    ]


def _index_events(trace: Mapping[str, Any]) -> dict[tuple[int, int], list[Mapping[str, Any]]]:
    indexed: dict[tuple[int, int], list[Mapping[str, Any]]] = {}
    for event in _records_of_kind(trace, "event"):
        if "episode_index" not in event or "episode_step" not in event:
            continue
        key = (int(event["episode_index"]), int(event["episode_step"]))
        indexed.setdefault(key, []).append(event)
    return indexed


def _task_row_events(task_row: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    transition = task_row.get("transition")
    if not isinstance(transition, Mapping):
        return []
    fault_code = transition.get("fault_code", 0)
    if not isinstance(fault_code, int | float) or fault_code == 0:
        return []
    return [
        {
            "event": "worker_fault",
            "episode_index": task_row.get("episode_index"),
            "episode_step": task_row.get("episode_step"),
            "fault_code": fault_code,
        }
    ]


def _row_alignment_mismatches(
    task: Mapping[str, Any], ue: Mapping[str, Any], required_fields: Sequence[str]
) -> list[str]:
    reasons: list[str] = []
    if task.get("phase") != ue.get("phase"):
        reasons.append(f"phase differs (Task={task.get('phase')!r}, UE={ue.get('phase')!r})")
    for task_clock, ue_clock, label in (
        ("input_observation_dt_s", "observation_dt_s", "input observation dt"),
        ("input_episode_elapsed_s", "episode_elapsed_s", "input episode time"),
    ):
        left = _nested(task, "clocks", task_clock)
        right = _nested(ue, "clocks", ue_clock)
        if left is None or right is None:
            reasons.append(f"{label} is missing (Task={left!r}, UE={right!r})")
        elif not _same_clock(left, right):
            reasons.append(f"{label} differs (Task={left!r}, UE={right!r})")
    reasons.extend(_command_mismatches(task, ue))
    reasons.extend(_row_shape_mismatches(task, ue, required_fields))
    return reasons


def _command_mismatches(task: Mapping[str, Any], ue: Mapping[str, Any]) -> list[str]:
    task_input = cast(Mapping[str, Any], task["input"])
    ue_input = cast(Mapping[str, Any], ue["input"])
    task_commands_value = task_input.get("commands", {})
    if not isinstance(task_commands_value, Mapping):
        return ["Task command values are not a channel mapping"]
    task_commands = cast(Mapping[str, Any], task_commands_value)
    ue_commands_value = ue_input.get("commands", [])
    if not isinstance(ue_commands_value, Sequence) or isinstance(ue_commands_value, str | bytes):
        return ["UE command values are not a sequence"]
    ue_commands: dict[str, Any] = {}
    reasons: list[str] = []
    for index, item in enumerate(ue_commands_value):
        if not isinstance(item, Mapping):
            reasons.append(f"UE command entry {index} is invalid")
            continue
        channel = item.get("channel")
        if not isinstance(channel, str) or not channel:
            reasons.append(f"UE command entry {index} has an invalid channel")
            continue
        if channel in ue_commands:
            reasons.append(f"UE command channel {channel!r} is repeated")
            continue
        ue_commands[channel] = item.get("values", ())
    if set(task_commands) != set(ue_commands):
        reasons.append(
            f"command channels differ (Task={sorted(task_commands)}, UE={sorted(ue_commands)})"
        )
    for channel in sorted(set(task_commands) & set(ue_commands)):
        left, left_error = _numeric_values(task_commands[channel], f"Task command {channel!r}")
        right, right_error = _numeric_values(ue_commands[channel], f"UE command {channel!r}")
        if left_error:
            reasons.append(left_error)
            continue
        if right_error:
            reasons.append(right_error)
            continue
        assert left is not None and right is not None
        if len(left) != len(right) or not all(
            _same_float32(a, b) for a, b in zip(left, right, strict=False)
        ):
            reasons.append(f"command {channel!r} differs (Task={left!r}, UE={right!r})")
    return reasons


def _row_metrics(
    task: Mapping[str, Any], ue: Mapping[str, Any], required_fields: Sequence[str]
) -> dict[str, object]:
    task_input = cast(Mapping[str, Any], task["input"])
    ue_input = cast(Mapping[str, Any], ue["input"])
    task_action = cast(Mapping[str, Any], task["action"])
    ue_action = cast(Mapping[str, Any], ue["action"])
    task_state = cast(Mapping[str, Sequence[float]], task_input["raw_state"])
    ue_state = _unpack_ue_state(ue_input)
    state_metrics = {
        name: _difference(task_state[name], ue_state[name])
        for name in required_fields
    }
    task_physical = cast(Mapping[str, Sequence[float]], task_action["physical_commands"])
    return {
        "input_raw_state": state_metrics,
        "input_observation": _difference(
            cast(Sequence[float], task_input["observation"]),
            cast(Sequence[float], ue_input["observation"]),
        ),
        "previous_action": _difference(
            cast(Sequence[float], task_input["previous_action"]),
            cast(Sequence[float], ue_input["previous_action"]),
        ),
        "policy_action": _difference(
            cast(Sequence[float], task_action["policy_action"]),
            cast(Sequence[float], ue_action["policy_action"]),
        ),
        "actuator_targets": _difference(
            task_physical[ROBOT_ACTUATOR_TARGET_FIELD],
            cast(Sequence[float], ue_action["actuator_targets"]),
        ),
    }


def _successor_comparison(
    task: Mapping[str, Any],
    ue_rows: Mapping[tuple[int, int], Mapping[str, Any]],
    required_fields: Sequence[str],
) -> dict[str, object]:
    """Compare pre-reset Task transition state to the next UE decision input."""

    transition = cast(Mapping[str, Any], task["transition"])
    if task.get("reset") is not None or transition.get("terminated") or transition.get("truncated"):
        return {"status": "reset_boundary", "reason": "terminal transition and reset are separate samples"}
    if transition.get("fault_code", 0):
        return {"status": "fault_boundary", "reason": "successor comparison stops at a policy/Worker fault"}
    key = (int(task["episode_index"]), int(task["episode_step"]) + 1)
    ue_next = ue_rows.get(key)
    if ue_next is None:
        return {"status": "unavailable", "reason": "next UE decision sample is missing"}
    reasons: list[str] = []
    task_dt = _nested(task, "clocks", "transition_dt_s")
    ue_dt = _nested(ue_next, "clocks", "physics_elapsed_s")
    if task_dt is None or ue_dt is None or not _same_clock(task_dt, ue_dt):
        reasons.append(f"transition window differs (Task={task_dt!r}, UE={ue_dt!r})")
    task_elapsed = _nested(task, "clocks", "transition_episode_elapsed_s")
    ue_elapsed = _nested(ue_next, "clocks", "episode_elapsed_s")
    if task_elapsed is None or ue_elapsed is None or not _same_clock(task_elapsed, ue_elapsed):
        reasons.append(f"successor episode time differs (Task={task_elapsed!r}, UE={ue_elapsed!r})")
    if ue_next.get("phase") != "post_window_input":
        reasons.append(f"successor phase is not post-window (UE={ue_next.get('phase')!r})")
    if reasons:
        return {"status": "not_comparable", "reasons": reasons}
    task_state_value = transition.get("raw_state")
    if not isinstance(task_state_value, Mapping):
        return {"status": "not_comparable", "reasons": ["Task successor raw state is invalid"]}
    task_state = cast(Mapping[str, Sequence[float]], task_state_value)
    ue_input = cast(Mapping[str, Any], ue_next["input"])
    try:
        ue_state = _unpack_ue_state(ue_input)
    except (KeyError, TypeError, ValueError) as exc:
        return {
            "status": "not_comparable",
            "reasons": [f"successor UE raw state layout is invalid ({exc})"],
        }
    state_reasons: list[str] = []
    for name in required_fields:
        if name not in task_state or name not in ue_state:
            state_reasons.append(f"successor deployment-required raw state field {name!r} is missing")
            continue
        task_values, task_error = _numeric_values(task_state[name], f"Task successor raw state {name!r}")
        ue_values, ue_error = _numeric_values(ue_state[name], f"UE successor raw state {name!r}")
        if task_error:
            state_reasons.append(task_error)
        if ue_error:
            state_reasons.append(ue_error)
        if task_values is not None and ue_values is not None and len(task_values) != len(ue_values):
            state_reasons.append(
                f"successor raw state field {name!r} width differs "
                f"(Task={len(task_values)}, UE={len(ue_values)})"
            )
    if state_reasons:
        return {"status": "not_comparable", "reasons": state_reasons}
    metrics = {
        name: _difference(task_state[name], ue_state[name])
        for name in required_fields
    }
    return {
        "status": "comparable",
        "actual_time_s": {
            "task_episode": task_elapsed,
            "ue_episode": ue_elapsed,
            "ue_solver": _nested(ue_next, "clocks", "solver_time_s"),
        },
        "raw_state": metrics,
    }


def _unpack_ue_state(ue_input: Mapping[str, Any]) -> dict[str, Sequence[float]]:
    fields_value = ue_input.get("raw_state_fields")
    values_value = ue_input.get("raw_state")
    if not isinstance(fields_value, Sequence) or isinstance(fields_value, str | bytes):
        raise ValueError("raw_state_fields must be a sequence")
    if not isinstance(values_value, Sequence) or isinstance(values_value, str | bytes):
        raise ValueError("raw_state must be a flat numeric sequence")
    fields = fields_value
    values = cast(Sequence[float], values_value)
    unpacked: dict[str, Sequence[float]] = {}
    offset = 0
    for index, field in enumerate(fields):
        if not isinstance(field, Mapping):
            raise ValueError(f"raw_state_fields entry {index} must be a mapping")
        name = field.get("name")
        width = field.get("width")
        if not isinstance(name, str) or not name:
            raise ValueError(f"raw_state_fields entry {index} has an invalid name")
        if name in unpacked:
            raise ValueError(f"raw_state_fields repeats {name!r}")
        if not isinstance(width, int) or isinstance(width, bool) or width < 1:
            raise ValueError(f"raw_state_fields entry {name!r} has an invalid width")
        if offset + width > len(values):
            raise ValueError(f"raw_state is too short for field {name!r}")
        unpacked[name] = values[offset : offset + width]
        offset += width
    if offset != len(values):
        raise ValueError(f"raw_state has {len(values) - offset} unassigned values")
    return unpacked


def _row_shape_mismatches(
    task: Mapping[str, Any], ue: Mapping[str, Any], required_fields: Sequence[str]
) -> list[str]:
    task_input = cast(Mapping[str, Any], task["input"])
    ue_input = cast(Mapping[str, Any], ue["input"])
    task_action = cast(Mapping[str, Any], task["action"])
    ue_action = cast(Mapping[str, Any], ue["action"])
    reasons: list[str] = []
    task_state_value = task_input.get("raw_state", {})
    if not isinstance(task_state_value, Mapping):
        return ["Task raw state is not a named mapping"]
    try:
        ue_state = _unpack_ue_state(ue_input)
    except (KeyError, TypeError, ValueError) as exc:
        return [f"UE raw state layout is invalid ({exc})"]
    task_state = cast(Mapping[str, Sequence[float]], task_state_value)
    for name in required_fields:
        if name not in task_state:
            reasons.append(f"deployment-required raw state field {name!r} is missing from Task input")
        if name not in ue_state:
            reasons.append(f"deployment-required raw state field {name!r} is missing from UE input")
        if name not in task_state or name not in ue_state:
            continue
        task_state_values, task_error = _numeric_values(
            task_state[name], f"Task raw state {name!r}"
        )
        ue_state_values, ue_error = _numeric_values(
            ue_state[name], f"UE raw state {name!r}"
        )
        if task_error:
            reasons.append(task_error)
        if ue_error:
            reasons.append(ue_error)
        if (
            task_state_values is not None
            and ue_state_values is not None
            and len(task_state_values) != len(ue_state_values)
        ):
            reasons.append(
                f"deployment-required raw state field {name!r} width differs "
                f"(Task={len(task_state_values)}, UE={len(ue_state_values)})"
            )
    task_physical = cast(Mapping[str, Sequence[float]], task_action.get("physical_commands", {}))
    task_targets = task_physical.get(ROBOT_ACTUATOR_TARGET_FIELD, ())
    comparisons = (
        ("observation", task_input.get("observation", ()), ue_input.get("observation", ())),
        ("previous_action", task_input.get("previous_action", ()), ue_input.get("previous_action", ())),
        ("policy_action", task_action.get("policy_action", ()), ue_action.get("policy_action", ())),
        ("actuator_targets", task_targets, ue_action.get("actuator_targets", ())),
    )
    for label, task_values, ue_values in comparisons:
        task_numbers, task_error = _numeric_values(task_values, f"Task {label}")
        ue_numbers, ue_error = _numeric_values(ue_values, f"UE {label}")
        if task_error:
            reasons.append(task_error)
        if ue_error:
            reasons.append(ue_error)
        if task_numbers is None or ue_numbers is None:
            continue
        if len(task_numbers) != len(ue_numbers):
            reasons.append(
                f"{label} width differs "
                f"(Task={len(task_numbers)}, UE={len(ue_numbers)})"
            )
    return reasons


def _required_state_fields(trace: Mapping[str, Any]) -> tuple[str, ...]:
    layout = trace.get("field_layout")
    if not isinstance(layout, Mapping):
        return ()
    required = layout.get("deployment_state_fields")
    if (
        not isinstance(required, Sequence)
        or isinstance(required, str | bytes)
        or any(not isinstance(name, str) for name in required)
    ):
        return ()
    return tuple(cast(Sequence[str], required))


def _numeric_values(value: object, label: str) -> tuple[list[float] | None, str | None]:
    if not isinstance(value, Sequence) or isinstance(value, str | bytes):
        return None, f"{label} is not a numeric sequence"
    numbers: list[float] = []
    for index, item in enumerate(value):
        if not isinstance(item, int | float) or isinstance(item, bool):
            return None, f"{label} value {index} is not numeric ({item!r})"
        number = float(item)
        if not math.isfinite(number):
            return None, f"{label} value {index} is non-finite ({item!r})"
        numbers.append(number)
    return numbers, None


def _difference(left: Sequence[float], right: Sequence[float]) -> dict[str, object]:
    deltas = [float(a) - float(b) for a, b in zip(left, right, strict=True)]
    if not all(math.isfinite(value) for value in [*left, *right]):
        return {"finite": False, "values": "contains non-finite value"}
    first = next(
        (
            {"index": index, "task": float(left[index]), "ue": float(right[index]), "delta": deltas[index]}
            for index in range(len(deltas))
            if deltas[index] != 0.0
        ),
        None,
    )
    return {
        "finite": True,
        "max_abs": max((abs(delta) for delta in deltas), default=0.0),
        "rms": math.sqrt(sum(delta * delta for delta in deltas) / len(deltas)) if deltas else 0.0,
        "first_difference": first,
    }


def _task_field_layout(
    records: Sequence[Mapping[str, object]],
    actor_observation_groups: Sequence[str],
    deployment_state_fields: Sequence[str],
) -> dict[str, object]:
    layout: dict[str, object] = {
        "deployment_state_fields": list(deployment_state_fields),
    }
    if not records:
        return layout
    first = records[0]
    input_row = cast(Mapping[str, Any], first["input"])
    action = cast(Mapping[str, Any], first["action"])
    physical_commands = cast(Mapping[str, Sequence[float]], action["physical_commands"])
    layout.update({
        "raw_state_fields": _named_widths(cast(Mapping[str, Sequence[object]], input_row["raw_state"])),
        "observation_groups": _named_widths(
            cast(Mapping[str, Sequence[object]], input_row["observation_groups"])
        ),
        "actor_observation_width": len(cast(Sequence[object], input_row["observation"])),
        "actor_observation_groups": list(actor_observation_groups),
        "previous_action_width": len(cast(Sequence[object], input_row["previous_action"])),
        "action_width": len(cast(Sequence[object], action["policy_action"])),
        "actuator_target_width": len(physical_commands[ROBOT_ACTUATOR_TARGET_FIELD]),
    })
    return layout


def _named_widths(values: Mapping[str, Sequence[object]]) -> list[dict[str, object]]:
    return [{"name": name, "width": len(value)} for name, value in values.items()]


def _nested(value: Mapping[str, Any], key: str, name: str) -> object:
    nested = value.get(key)
    return nested.get(name) if isinstance(nested, Mapping) else None


def _same_number(left: object, right: object) -> bool:
    if not isinstance(left, (int, float, str)) or not isinstance(right, (int, float, str)):
        return False
    try:
        left_number = float(left)
        right_number = float(right)
    except (TypeError, ValueError):
        return False
    return math.isfinite(left_number) and math.isfinite(right_number) and left_number == right_number


def _same_clock(left: object, right: object) -> bool:
    """Accept solver precision, including a double sum of binary32 step durations."""

    if _same_number(left, right) or _same_float32(left, right):
        return True
    if not isinstance(left, (int, float, str)) or not isinstance(right, (int, float, str)):
        return False
    try:
        left_number, right_number = float(left), float(right)
    except (TypeError, ValueError):
        return False
    # Chaos quantizes each solver dt to binary32 before accumulating its double
    # clock. The relative rounding bound is 2**-24; casting only the accumulated
    # total can land on the other side of a binary32 rounding boundary. Retain
    # raw timestamps and the stricter float32 comparison for command values.
    return (
        math.isfinite(left_number)
        and math.isfinite(right_number)
        and math.isclose(left_number, right_number, rel_tol=2**-24, abs_tol=0.0)
    )


def _same_float32(left: object, right: object) -> bool:
    if not isinstance(left, (int, float, str)) or not isinstance(right, (int, float, str)):
        return False
    try:
        left_number = float(left)
        right_number = float(right)
        if not math.isfinite(left_number) or not math.isfinite(right_number):
            return False
        return struct.pack("!f", left_number) == struct.pack("!f", right_number)
    except (OverflowError, TypeError, ValueError):
        return False


def _yaml_value(value: object) -> object:
    if isinstance(value, Path):
        return value.as_posix()
    if isinstance(value, Mapping):
        return {str(key): _yaml_value(item) for key, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_yaml_value(item) for item in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    raise TypeError(f"trace value is not a YAML scalar/container: {type(value).__name__}")

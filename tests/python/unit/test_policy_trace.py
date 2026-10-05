"""Verify Task/UE decision trace files and conservative pairing."""

from __future__ import annotations

from pathlib import Path
from typing import Any, cast

import pytest
import yaml

from uerl.core.config.yaml_loader import load_unique_yaml
from uerl.core.direct.robot_action import ROBOT_ACTUATOR_TARGET_FIELD
from uerl.training.policy_trace import TaskPolicyTraceRecorder, compare_policy_traces


def _task_row(*, episode_step: int, phase: str, time_s: float = 0.01) -> dict[str, object]:
    return {
        "kind": "step",
        "episode_index": 0,
        "episode_step": episode_step,
        "phase": phase,
        "clocks": {
            "physics_dt_s": 0.005,
            "input_observation_dt_s": 0.01,
            "step_decimation": 2,
            "transition_dt_s": 0.01,
            "input_episode_elapsed_s": time_s,
            "transition_episode_elapsed_s": time_s + 0.01,
        },
        "input": {
            "raw_state": {"state.value": [1.0 + time_s]},
            "state_valid": True,
            "fault_code": 0,
            "observation_groups": {"policy": [1.0 + time_s, 0.25]},
            "observation": [1.0 + time_s, 0.25],
            "previous_action": [0.2],
            "commands": {"velocity": [0.4, 0.0, 0.25]},
        },
        "action": {
            "policy_action": [0.3],
            "physical_commands": {ROBOT_ACTUATOR_TARGET_FIELD: [2.0]},
        },
        "transition": {
            "raw_state": {"state.value": [1.1 + time_s]},
            "state_valid": True,
            "fault_code": 0,
            "observation_groups": {"policy": [1.1 + time_s, 0.25]},
            "reward": 1.0,
            "terminated": False,
            "truncated": False,
            "termination_reason": {},
        },
        "reset": None,
    }


def _ue_trace(
    *,
    episode_step: int,
    phase: str,
    time_s: float = 0.01,
    command: float = 0.4,
    observation_dt_s: float = 0.01,
) -> dict[str, object]:
    return {
        "schema_version": 1,
        "source": {
            "side": "ue",
            "task_id": "test.task",
            "robot_id": "test.robot",
            "artifact_asset": "/Game/Policies/Test.Test",
            "map_package": "/Game/Maps/Test",
            "seed": 7,
            "policy_onnx_sha1": "a" * 40,
        },
        "clock": {"physics_dt_s": 0.005, "decimation_min": 1, "decimation_max": 7},
        "field_layout": {
            "raw_state_fields": [{"name": "state.value", "width": 1}],
            "observation_width": 2,
            "previous_action_width": 1,
            "action_width": 1,
            "actuator_target_width": 1,
        },
        "status": "complete",
        "records": [
            {
                "kind": "frame",
                "sequence": episode_step + 1,
                "episode_index": 0,
                "episode_step": episode_step,
                "phase": phase,
                "clocks": {
                    "solver_time_s": 100.0 + time_s,
                    "episode_elapsed_s": time_s,
                    "physics_elapsed_s": 0.01,
                    "game_elapsed_s": 0.01,
                    "observation_dt_s": observation_dt_s,
                    "last_solver_step_dt_s": 0.005,
                },
                "input": {
                    "raw_state_fields": [{"name": "state.value", "width": 1}],
                    "raw_state": [1.0 + time_s],
                    "observation": [1.0 + time_s, 0.25],
                    "previous_action": [0.2],
                    "commands": [{"channel": "velocity", "values": [command, 0.0, 0.25], "age_seconds": 0.0}],
                },
                "action": {"policy_action": [0.3], "actuator_targets": [2.0]},
            }
        ],
    }


def _write_task_trace(path: Path, row: dict[str, object]) -> None:
    recorder = TaskPolicyTraceRecorder(
        path,
        source={
            "task_id": "test.task",
            "robot_id": "test.robot",
            "run_directory": "runs/test",
            "checkpoint": "model.pt",
            "map_package": "/Game/Maps/Test",
            "seed": 7,
            "policy_onnx_sha1": "a" * 40,
            "controller": "task",
        },
        clock={"physics_dt_s": 0.005, "decimation_range": [1, 7]},
        actor_observation_groups=("policy",),
    )
    recorder.record_step(row)
    recorder.finish(complete=True)


def test_task_trace_is_yaml_and_names_input_transition_and_reset_phases(tmp_path: Path) -> None:
    trace_path = tmp_path / "run" / "traces" / "task.yaml"
    row = _task_row(episode_step=0, phase="post_reset_input", time_s=0.0)
    row["reset"] = {
        "raw_state": {"state.value": [0.0]},
        "observation_groups": {"policy": [0.0, 0.0]},
        "episode_index": 1,
    }

    _write_task_trace(trace_path, row)
    payload = cast(dict[str, Any], load_unique_yaml(trace_path.read_text(encoding="utf-8")))

    assert payload["schema_version"] == 1
    assert payload["source"]["side"] == "task"
    assert payload["field_layout"]["raw_state_fields"] == [{"name": "state.value", "width": 1}]
    assert payload["field_layout"]["actor_observation_width"] == 2
    assert payload["status"] == "complete"
    saved = payload["records"][0]
    assert saved["input"]["raw_state"] == {"state.value": [1.0]}
    assert saved["transition"]["raw_state"] == {"state.value": [1.1]}
    assert saved["reset"]["raw_state"] == {"state.value": [0.0]}


def test_trace_pairing_reports_bootstrap_phase_as_not_comparable(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=0, phase="post_reset_input", time_s=0.0))
    ue_path.write_text(yaml.safe_dump(_ue_trace(episode_step=0, phase="bootstrap_input")), encoding="utf-8")

    report = compare_policy_traces(task_path, ue_path)

    assert report["status"] == "not_comparable"
    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["comparable"] is False
    assert any("phase" in reason for reason in pair["reasons"])


def test_trace_pairing_reports_clock_and_command_mismatch_with_actual_values(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=2, phase="post_window_input", time_s=0.02))
    ue_path.write_text(
        yaml.safe_dump(_ue_trace(episode_step=2, phase="post_window_input", time_s=0.02, command=0.5,
                                 observation_dt_s=0.015)),
        encoding="utf-8",
    )

    report = compare_policy_traces(task_path, ue_path)

    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["comparable"] is False
    assert any("command" in reason and "0.4" in reason and "0.5" in reason for reason in pair["reasons"])
    assert any("observation dt" in reason and "0.01" in reason and "0.015" in reason for reason in pair["reasons"])


def test_trace_pairing_requires_the_same_exported_policy(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=2, phase="post_window_input", time_s=0.02))
    ue = _ue_trace(episode_step=2, phase="post_window_input", time_s=0.02)
    cast(dict[str, Any], ue["source"])["policy_onnx_sha1"] = "b" * 40
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")

    report = compare_policy_traces(task_path, ue_path)

    assert report["status"] == "not_comparable"
    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert any("policy ONNX fingerprint differs" in reason for reason in pair["reasons"])


def test_trace_pairing_attaches_timing_events_to_their_policy_frame(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=2, phase="post_window_input", time_s=0.02))
    ue = _ue_trace(episode_step=2, phase="post_window_input", time_s=0.02)
    records = cast(list[dict[str, Any]], ue["records"])
    records.append(
        {
            "kind": "event",
            "event": "command_stale",
            "sequence": 3,
            "episode_index": 0,
            "episode_step": 2,
            "channel": "velocity",
            "stale_seconds": 0.02,
        }
    )
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")

    report = compare_policy_traces(task_path, ue_path)

    assert report["status"] == "comparable"
    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["events"]["ue"][0]["event"] == "command_stale"


def test_policy_fault_is_a_faulted_trace_and_attaches_to_the_failed_frame(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=0, phase="post_reset_input", time_s=0.0))
    ue = _ue_trace(episode_step=0, phase="bootstrap_input", time_s=0.0)
    ue["status"] = "faulted"
    ue["records"] = [
        {
            "kind": "event",
            "event": "policy_fault",
            "sequence": 1,
            "episode_index": 0,
            "episode_step": 0,
            "reason": "stale command",
        }
    ]
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")

    report = compare_policy_traces(task_path, ue_path)

    assert report["status"] == "faulted"
    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["comparable"] is False
    assert pair["events"]["ue"][0]["event"] == "policy_fault"


def test_task_worker_fault_marks_the_pair_report_faulted(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    task_row = _task_row(episode_step=2, phase="post_window_input", time_s=0.02)
    cast(dict[str, Any], task_row["transition"])["fault_code"] = 19
    _write_task_trace(task_path, task_row)
    ue_path.write_text(
        yaml.safe_dump(_ue_trace(episode_step=2, phase="post_window_input", time_s=0.02)),
        encoding="utf-8",
    )

    report = compare_policy_traces(task_path, ue_path)

    assert report["status"] == "faulted"
    assert report["faulted_sources"] == ["task"]
    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["comparable"] is True  # The captured input/action pair remains useful diagnostic evidence.
    assert pair["events"]["task"][0]["event"] == "worker_fault"


def test_unmarked_bootstrap_event_attaches_to_the_affected_frame(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=2, phase="post_window_input", time_s=0.02))
    ue = _ue_trace(episode_step=2, phase="bootstrap_input", time_s=0.02)
    records = cast(list[dict[str, Any]], ue["records"])
    records.append(
        {
            "kind": "event",
            "event": "unmarked_bootstrap_boundary",
            "sequence": 3,
            "episode_index": 0,
            "episode_step": 2,
        }
    )
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")

    report = compare_policy_traces(task_path, ue_path)

    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["events"]["ue"][0]["event"] == "unmarked_bootstrap_boundary"
    assert pair["comparable"] is False  # The bootstrap phase still prevents a false alignment.


def test_incomplete_trace_cannot_be_compared_as_complete(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=2, phase="post_window_input", time_s=0.02))
    ue = _ue_trace(episode_step=2, phase="post_window_input", time_s=0.02)
    ue["status"] = "incomplete"
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")

    with pytest.raises(ValueError, match="incomplete"):
        compare_policy_traces(task_path, ue_path)


def test_aligned_trace_differences_are_diagnostics_without_a_quality_verdict(tmp_path: Path) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=2, phase="post_window_input", time_s=0.02))
    ue = _ue_trace(episode_step=2, phase="post_window_input", time_s=0.02)
    frame = cast(list[dict[str, Any]], ue["records"])[0]
    frame["input"]["raw_state"] = [1.03]
    frame["input"]["observation"] = [1.03, 0.25]
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")

    report = compare_policy_traces(task_path, ue_path)

    assert report["status"] == "comparable"
    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["comparable"] is True
    assert pair["metrics"]["input_raw_state"]["state.value"]["max_abs"] == pytest.approx(0.01)
    assert pair["metrics"]["input_observation"]["max_abs"] == pytest.approx(0.01)
    assert "threshold" not in pair


def test_clock_pairing_accepts_same_float32_solver_value_but_reports_real_mismatch(
    tmp_path: Path,
) -> None:
    task_path = tmp_path / "task.yaml"
    ue_path = tmp_path / "ue.yaml"
    _write_task_trace(task_path, _task_row(episode_step=2, phase="post_window_input", time_s=0.02))
    ue = _ue_trace(episode_step=2, phase="post_window_input", time_s=0.02)
    frame = cast(list[dict[str, Any]], ue["records"])[0]
    frame["clocks"]["observation_dt_s"] = 0.0100000001
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")

    report = compare_policy_traces(task_path, ue_path)

    pair = cast(list[dict[str, Any]], report["pairs"])[0]
    assert pair["comparable"] is True

    frame["clocks"]["episode_elapsed_s"] = 0.02001
    ue_path.write_text(yaml.safe_dump(ue), encoding="utf-8")
    mismatched = compare_policy_traces(task_path, ue_path)
    mismatch_pair = cast(list[dict[str, Any]], mismatched["pairs"])[0]
    assert mismatch_pair["comparable"] is False
    assert any(
        "input episode time" in reason and "0.02" in reason and "0.02001" in reason
        for reason in mismatch_pair["reasons"]
    )


def test_trace_recorder_does_not_overwrite_existing_evidence(tmp_path: Path) -> None:
    trace_path = tmp_path / "trace.yaml"
    trace_path.write_text("existing evidence\n", encoding="utf-8")

    with pytest.raises(FileExistsError, match="trace already exists"):
        TaskPolicyTraceRecorder(
            trace_path,
            source={"task_id": "test.task"},
            clock={"physics_dt_s": 0.005},
            actor_observation_groups=("policy",),
        )

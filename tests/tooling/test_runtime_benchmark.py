"""Check benchmark accounting and ownership without claiming runtime throughput."""

from pathlib import Path
from types import SimpleNamespace
from typing import cast
from unittest.mock import Mock

import pytest
import torch
import yaml

from scripts import benchmark_runtime
from uerl import UERLDirectEnv, UERLSession


def _step(dt: float, *, invalid: bool = False, done: bool = False) -> tuple[object, ...]:
    return (
        {},
        torch.zeros(4),
        torch.tensor([done, False, False, False]),
        torch.zeros(4, dtype=torch.bool),
        {
            "terminal_observation_valid": torch.full((4,), not invalid, dtype=torch.bool),
            "slot_fault_code": torch.full((4,), 2 if invalid else 0, dtype=torch.long),
            "transition_dt": dt,
            "episode_metrics": {"Perf/stage_step_roundtrip": 0.002},
        },
    )


def _env() -> Mock:
    return Mock(num_envs=4, task=SimpleNamespace(num_actions=1), device=torch.device("cpu"))


def test_summary_distinguishes_slots_control_steps_and_physical_time() -> None:
    result = benchmark_runtime.summarize([0.01, 0.03, 0.02], wall_seconds=0.12, simulated_seconds=0.06, slots=4)
    assert result["measured_steps"] == 3
    assert result["slot_transitions"] == 12
    assert result["control_steps_per_wall_second"] == 25.0
    assert result["slot_transitions_per_wall_second"] == 100.0
    assert result["physical_seconds_per_wall_second"] == 0.5
    assert result["step_latency_s"] == pytest.approx({"mean": 0.02, "median": 0.02, "p95": 0.029})


def test_measure_excludes_warmup_and_preserves_actual_control_intervals() -> None:
    env = _env()
    env.step.side_effect = [_step(10.0), _step(0.005), _step(0.020, done=True), _step(0.035)]
    timestamps = iter([0.0, 0.0, 0.01, 0.02, 0.05, 0.06, 0.08, 0.12])
    report = benchmark_runtime.measure(
        cast(UERLDirectEnv, env), steps=3, warmup_steps=1, clock=lambda: next(timestamps)
    )
    assert env.step.call_count == 4
    for call in env.step.call_args_list:
        assert torch.equal(call.args[0], torch.zeros(4, 1))
    assert report["physical_seconds_per_slot"] == pytest.approx(0.06)
    assert report["slot_transitions_per_wall_second"] == pytest.approx(100.0)
    assert report["completed_episodes"] == 1
    assert report["stage_mean_s"] == {"Perf/stage_step_roundtrip": 0.002}


@pytest.mark.parametrize("warmup", [0, 1])
def test_measure_rejects_faulted_rows_instead_of_reporting_high_throughput(warmup: int) -> None:
    env = _env()
    env.step.return_value = _step(0.02, invalid=True)
    with pytest.raises(RuntimeError, match="invalid or faulted Slot"):
        benchmark_runtime.measure(cast(UERLDirectEnv, env), steps=2, warmup_steps=warmup)
    assert env.step.call_count == 1


def test_existing_report_is_not_overwritten_or_followed_by_worker_launch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    path = tmp_path / "report.yaml"
    path.write_text("preserve", encoding="utf-8")
    opener = Mock()
    monkeypatch.setattr(UERLSession, "open", opener)
    with pytest.raises(SystemExit) as error:
        benchmark_runtime.main(["--task", "UERL-CartPole-Direct-v0", "--output", str(path)])
    assert error.value.code == 2
    assert path.read_text(encoding="utf-8") == "preserve"
    opener.assert_not_called()


def test_setup_failure_closes_owned_session_and_writes_failed_yaml(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    raw = Mock()
    monkeypatch.setattr(UERLSession, "open", Mock(return_value=raw))
    monkeypatch.setattr(benchmark_runtime, "UERLSessionAdapter", Mock())
    monkeypatch.setattr(benchmark_runtime, "UERLDirectEnv", Mock(side_effect=RuntimeError("initialization failed")))
    monkeypatch.setattr(
        benchmark_runtime,
        "resolve_host_flags",
        lambda args: SimpleNamespace(
            ue_executable=tmp_path / "UE.exe",
            project=tmp_path / "Host.uproject",
        ),
    )
    path = tmp_path / "report.yaml"
    result = benchmark_runtime.main(["--task", "UERL-CartPole-Direct-v0", "--output", str(path)])
    assert result == 1
    raw.close.assert_called_once_with("runtime_benchmark_setup_failed")
    report = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert report["status"] == "FAIL"
    assert report["shutdown"] == "PASS"
    assert report["error"] == "RuntimeError: initialization failed"
    assert report["training"] is False
    assert "slot_transitions_per_wall_second" not in report


def test_nonfinite_task_output_fails_even_when_worker_flags_are_valid() -> None:
    env = _env()
    observations, rewards, terminated, truncated, info = _step(0.02)
    env.step.return_value = ({"policy": torch.full((4, 1), float("nan"))}, rewards, terminated, truncated, info)
    with pytest.raises(RuntimeError, match="non-finite observation or reward"):
        benchmark_runtime.measure(cast(UERLDirectEnv, env), steps=1, warmup_steps=0)

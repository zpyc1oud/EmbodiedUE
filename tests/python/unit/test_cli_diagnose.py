"""Verify the unified ``uerl diagnose`` CLI command group."""

from __future__ import annotations

from pathlib import Path

import pytest
import yaml

from uerl.cli.main import main


def test_diagnose_training_debug_outputs_yaml(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    trace = tmp_path / "steps.yaml"
    trace.write_text("kind: header\n", encoding="utf-8")
    seen: list[Path] = []

    def fake_audit(path: Path) -> dict[str, object]:
        seen.append(path)
        return {"steps": 4, "valid": True}

    monkeypatch.setattr("uerl.training.debug_audit.audit_training_trace", fake_audit)

    assert main(["diagnose", "training-debug", str(trace)]) == 0
    assert seen == [trace]
    assert yaml.safe_load(capsys.readouterr().out) == {"steps": 4, "valid": True}


@pytest.mark.parametrize(("status", "expected_code"), [("comparable", 0), ("not_comparable", 2)])
def test_diagnose_compare_traces_returns_status_exit_code(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    status: str,
    expected_code: int,
) -> None:
    task_trace = tmp_path / "task.yaml"
    ue_trace = tmp_path / "ue.yaml"

    def fake_compare(lhs: Path, rhs: Path) -> dict[str, object]:
        assert lhs == task_trace and rhs == ue_trace
        return {"status": status, "pairs": 1}

    monkeypatch.setattr("uerl.training.policy_trace.compare_policy_traces", fake_compare)

    assert main(["diagnose", "compare-traces", str(task_trace), str(ue_trace)]) == expected_code
    assert yaml.safe_load(capsys.readouterr().out) == {"status": status, "pairs": 1}


def test_diagnose_compare_traces_handles_errors(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    def failing_compare(lhs: Path, rhs: Path) -> dict[str, object]:
        del lhs, rhs
        raise ValueError("mismatched metadata")

    monkeypatch.setattr("uerl.training.policy_trace.compare_policy_traces", failing_compare)

    assert main(["diagnose", "compare-traces", str(tmp_path / "a.yaml"), str(tmp_path / "b.yaml")]) == 2
    assert "Trace comparison failed: mismatched metadata" in capsys.readouterr().err


def test_diagnose_training_trace_forwards_options_and_events(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    trace = tmp_path / "task.yaml"
    events = tmp_path / "events"
    captured: dict[str, object] = {}

    def fake_diagnose(
        path: Path,
        *,
        body: str,
        command: str,
        low_motion_speed: float,
    ) -> dict[str, object]:
        captured.update(path=path, body=body, command=command, low_motion_speed=low_motion_speed)
        return {"steps": 12}

    def fake_scalars(events_dir: Path, *, last_iterations: int) -> dict[str, object]:
        captured.update(events_dir=events_dir, last_iterations=last_iterations)
        return {"scalars": {"Train/mean_reward": {"latest": 1.0}}}

    monkeypatch.setattr("uerl.training.trace_diagnostics.diagnose_task_trace", fake_diagnose)
    monkeypatch.setattr("uerl.training.trace_diagnostics.summarize_training_scalars", fake_scalars)

    result = main(
        [
            "diagnose",
            "training-trace",
            str(trace),
            "--body",
            "chassis",
            "--command",
            "vel",
            "--low-motion-speed",
            "0.05",
            "--events",
            str(events),
            "--last-iterations",
            "25",
        ]
    )

    assert result == 0
    assert captured == {
        "path": trace,
        "body": "chassis",
        "command": "vel",
        "low_motion_speed": 0.05,
        "events_dir": events,
        "last_iterations": 25,
    }
    payload = yaml.safe_load(capsys.readouterr().out)
    assert payload["steps"] == 12
    assert payload["training_scalars"]["scalars"]["Train/mean_reward"]["latest"] == 1.0


def test_diagnose_requires_valid_subcommand() -> None:
    with pytest.raises(SystemExit) as exc:
        main(["diagnose", "nonexistent"])
    assert exc.value.code == 2

"""Verify user-input failures print one actionable line instead of raising."""

from __future__ import annotations

from pathlib import Path

import pytest

from uerl.application.run_config import RunConfig
from uerl.cli import config as config_cli
from uerl.cli import export as export_cli
from uerl.cli import play as play_cli
from uerl.cli import train as train_cli
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


def test_unknown_task_reports_candidates_before_launching_ue(capsys) -> None:  # type: ignore[no-untyped-def]
    assert train_cli.main(["--task", "UERL-PhantomX-Wlak-v0"]) == 1

    output = capsys.readouterr().out
    assert "[FAIL] unknown Task 'UERL-PhantomX-Wlak-v0'" in output
    assert "uerl tasks" in output
    assert "UERL-PhantomX-Walk-v0" in output


def test_unknown_override_path_names_the_path_and_the_preview_command(capsys) -> None:  # type: ignore[no-untyped-def]
    assert config_cli.main(["--task", "UERL-PhantomX-Walk-v0", "--runner.devcie", "cuda:0"]) == 1

    output = capsys.readouterr().out
    assert "[FAIL] config --runner.devcie" in output
    assert "uerl config" in output


def test_missing_run_directory_reports_the_resolved_path(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    assert play_cli.main(["--task", "UERL-PhantomX-Walk-v0", "--run", str(tmp_path / "absent")]) == 1

    assert "[FAIL] Run directory not found" in capsys.readouterr().out


def test_export_missing_robot_runtime_reports_yaml_input(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    checkpoint = tmp_path / "model_final.pt"
    checkpoint.write_bytes(b"checkpoint placeholder")
    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={"session.mode": "attach", "worker.slot_count": "1"},
    )

    class RunConfigSource:
        def resolve(self, overrides: dict[str, str]) -> RunConfig:
            del overrides
            return RunConfig(config, config.normalized_hash, False, "test Run config")

    source = RunConfigSource()
    monkeypatch.setattr(export_cli, "find_run_directory_for_checkpoint", lambda _: None)
    monkeypatch.setattr(export_cli, "load_run_config_source", lambda *args, **kwargs: source)

    with pytest.raises(SystemExit) as error:
        export_cli.main(
            [
                "--checkpoint",
                str(checkpoint),
                "--output",
                str(tmp_path / "policy.uerlpol2"),
                "--session.mode",
                "attach",
                "--robot-runtime",
                str(tmp_path / "missing-runtime.yaml"),
            ]
        )

    assert error.value.code == 2
    output = capsys.readouterr().err
    assert "--robot-runtime must point to an existing YAML file" in output
    assert "existing YAML or JSON file" not in output

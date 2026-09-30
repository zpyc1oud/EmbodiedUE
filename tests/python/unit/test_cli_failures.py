"""Verify user-input failures print one actionable line instead of raising."""

from __future__ import annotations

from pathlib import Path

from uerl.cli import config as config_cli
from uerl.cli import play as play_cli
from uerl.cli import train as train_cli


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

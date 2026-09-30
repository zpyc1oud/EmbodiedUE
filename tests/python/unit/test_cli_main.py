"""Verify the unified command dispatcher stays a thin adapter."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from uerl.cli import main as cli


def test_unified_tasks_command_forwards_arguments(capsys) -> None:  # type: ignore[no-untyped-def]
    assert cli.main(["tasks", "--filter", "CartPole", "--json"]) == 0

    rows = json.loads(capsys.readouterr().out)
    assert [row["task_id"] for row in rows] == ["UERL-CartPole-Direct-v0"]


def test_unified_command_without_subcommand_prints_help(capsys) -> None:  # type: ignore[no-untyped-def]
    assert cli.main([]) == 0
    assert "uerl" in capsys.readouterr().out


def test_unified_command_help_flag_prints_help(capsys) -> None:  # type: ignore[no-untyped-def]
    assert cli.main(["--help"]) == 0
    assert "uerl" in capsys.readouterr().out


def test_unified_new_command_forwards_arguments(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    assert (
        cli.main(
            [
                "new",
                "robot",
                "arm2",
                "--asset",
                "/Game/Robots/Arm2/SK_Arm2",
                "--output-dir",
                str(tmp_path),
            ]
        )
        == 0
    )
    assert (tmp_path / "src" / "uerl" / "assets" / "robots" / "arm2.py").exists()
    capsys.readouterr()


def test_unified_command_rejects_unknown_subcommand() -> None:
    with pytest.raises(SystemExit):
        cli.main(["unknown"])

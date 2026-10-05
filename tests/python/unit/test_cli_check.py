"""Verify pure-Python Task preflight output and failure suggestions."""

from __future__ import annotations

import json

from uerl.cli import check
from uerl.tasks.cartpole import CARTPOLE_TASK_ID


def test_task_check_json_resolves_registry_config_and_runtime(capsys) -> None:  # type: ignore[no-untyped-def]
    assert check.main(["task", CARTPOLE_TASK_ID, "--json"]) == 0

    report = json.loads(capsys.readouterr().out)
    assert report["task_id"] == CARTPOLE_TASK_ID
    assert report["actuator_count"] == 1
    assert len(report["config_hash"]) == 64
    assert report["capabilities"]["train"]["status"] == "unknown"
    assert report["capabilities"]["evaluate"]["status"] == "unknown"
    assert report["capabilities"]["export"]["status"] == "unknown"
    assert "RobotSpec" in report["capabilities"]["export"]["reason"]


def test_task_check_text_reports_capabilities(capsys) -> None:  # type: ignore[no-untyped-def]
    assert check.main(["task", CARTPOLE_TASK_ID]) == 0

    output = capsys.readouterr().out
    assert "capabilities train=unknown" in output
    assert "evaluate=unknown" in output
    assert "export=unknown" in output


def test_task_check_unknown_id_returns_candidates(capsys) -> None:  # type: ignore[no-untyped-def]
    assert check.main(["task", "UERL-CartPole-Direct-v9"]) == 1

    output = capsys.readouterr().out
    assert "unknown Task" in output
    assert "UERL-CartPole-Direct-v0" in output
    assert "uerl tasks" in output

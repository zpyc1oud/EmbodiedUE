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


def test_task_check_unknown_id_returns_candidates(capsys) -> None:  # type: ignore[no-untyped-def]
    assert check.main(["task", "UERL-CartPole-Direct-v9"]) == 1

    output = capsys.readouterr().out
    assert "unknown Task" in output
    assert "UERL-CartPole-Direct-v0" in output
    assert "uerl tasks" in output

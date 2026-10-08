"""Verify the no-UE task and configuration discovery commands."""

from __future__ import annotations

import json

from uerl.cli import config as config_cli
from uerl.cli import tasks as tasks_cli
from uerl.tasks.cartpole import CARTPOLE_TASK_ID


def test_tasks_json_lists_stable_filtered_rows(capsys) -> None:  # type: ignore[no-untyped-def]
    assert tasks_cli.main(["--filter", "phantomx", "--json"]) == 0

    rows = json.loads(capsys.readouterr().out)
    assert {row["task_id"] for row in rows} == {
        "UERL-PhantomX-Walk-v0", "UERL-PhantomX-Pursuit-v0",
        "UERL-PhantomX-ContinuousTerrain-v0", "UERL-PhantomX-DiscreteTerrain-v0",
    }
    assert all("phantomx" in row["task_id"].casefold() for row in rows)
    assert [row["task_id"] for row in rows] == sorted(row["task_id"] for row in rows)
    assert {"task_id", "robot_id", "environment_id", "slot_count", "map_path"} <= set(rows[0])


def test_tasks_table_identifies_the_robot_by_asset(capsys) -> None:  # type: ignore[no-untyped-def]
    assert tasks_cli.main(["--filter", "phantomx"]) == 0

    table = capsys.readouterr().out
    assert "SK_PhantomX" in table
    assert "uerl.robot.skeletal_mesh" not in table


def test_config_json_uses_the_typed_resolver(capsys) -> None:  # type: ignore[no-untyped-def]
    assert config_cli.main(["--task", CARTPOLE_TASK_ID, "--runner.max_iterations", "3", "--json"]) == 0

    config = json.loads(capsys.readouterr().out)
    assert config["task_id"] == CARTPOLE_TASK_ID
    assert config["runner"]["max_iterations"] == 3
    assert len(config["normalized_hash"]) == 64

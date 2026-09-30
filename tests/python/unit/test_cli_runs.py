"""Verify `uerl runs` lists task-scoped Runs without starting Unreal Engine."""

from __future__ import annotations

import json
from pathlib import Path

from uerl.cli import runs


def _write_run(root: Path, task_id: str, name: str, checkpoint: str) -> None:
    run_directory = root / task_id / name
    if checkpoint == "model_final.pt":
        run_directory.mkdir(parents=True)
        (run_directory / checkpoint).write_bytes(b"final")
    else:
        saved = run_directory / "rsl_rl"
        saved.mkdir(parents=True)
        (saved / checkpoint).write_bytes(b"intermediate")
    (run_directory / "command.txt").write_text("train\n", encoding="utf-8")


def test_runs_lists_newest_first_with_the_resumable_checkpoint(tmp_path: Path, capsys) -> None:  # type: ignore[no-untyped-def]
    task_id = "UERL-PhantomX-Walk-v0"
    _write_run(tmp_path, task_id, "20260923-140000-walk", "model_50.pt")
    _write_run(tmp_path, task_id, "20260923-150000-walk", "model_final.pt")
    _write_run(tmp_path, "UERL-CartPole-Direct-v0", "20260923-160000-smoke", "model_final.pt")

    assert runs.main(["--root", str(tmp_path), "--task", task_id, "--json"]) == 0

    rows = json.loads(capsys.readouterr().out)
    assert [row["checkpoint"] for row in rows] == ["model_final.pt", "rsl_rl/model_50.pt"]
    assert all(row["task_id"] == task_id for row in rows)

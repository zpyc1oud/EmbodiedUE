"""Verify the training CLI's explicit Run and resume contract."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from uerl import training
from uerl.cli import train
from uerl.tasks.cartpole import CARTPOLE_TASK_ID


def test_train_resume_resolves_source_and_writes_new_run_command(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    source = tmp_path / "source-run"
    source.mkdir()
    source_checkpoint = source / "model_final.pt"
    source_checkpoint.write_bytes(b"source")
    output = tmp_path / "new-run"
    captured = []

    def fake_run(config, *, terrain_level=None):  # type: ignore[no-untyped-def]
        captured.append(config)
        return SimpleNamespace(
            task_id=config.task_id,
            iterations=1,
            checkpoint=output / "model_final.pt",
            metrics_directory=output / "rsl_rl",
            normalized_hash=config.normalized_hash,
        )

    monkeypatch.setattr(training, "run_training", fake_run)

    assert train.main(
        [
            "--task",
            CARTPOLE_TASK_ID,
            "--session.mode",
            "attach",
            "--run-dir",
            str(output),
            "--resume",
            str(source),
        ]
    ) == 0

    assert captured[0].logging.run_directory == output
    assert captured[0].runner.checkpoint == source_checkpoint
    assert (output / "command.txt").is_file()

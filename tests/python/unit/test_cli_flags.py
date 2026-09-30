"""Verify everyday flags write the same config fields as dotted overrides."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from uerl import training
from uerl.cli import train
from uerl.tasks.cartpole import CARTPOLE_TASK_ID


def test_train_common_flags_override_task_defaults(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    captured = []

    def fake_run(config, *, terrain_level=None):  # type: ignore[no-untyped-def]
        del terrain_level
        captured.append(config)
        return SimpleNamespace(
            task_id=config.task_id,
            iterations=config.runner.max_iterations,
            checkpoint=tmp_path / "model_final.pt",
            metrics_directory=tmp_path / "rsl_rl",
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
            str(tmp_path),
            "--num-envs",
            "8",
            "--seed",
            "3",
            "--device",
            "cuda:0",
            "--max-iterations",
            "2",
        ]
    ) == 0

    config = captured[0]
    assert config.worker.slot_count == 8
    assert config.worker.run_seed == 3
    assert config.runner.device == "cuda:0"
    assert config.runner.max_iterations == 2


def test_common_flag_rejects_the_same_dotted_path() -> None:
    with pytest.raises(SystemExit):
        train.main(
            [
                "--task",
                CARTPOLE_TASK_ID,
                "--device",
                "cpu",
                "--runner.device",
                "cuda:0",
            ]
        )

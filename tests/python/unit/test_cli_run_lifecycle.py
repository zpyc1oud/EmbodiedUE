"""Verify the training CLI's explicit Run and resume contract."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest
import torch

from uerl import training
from uerl.cli import train
from uerl.core.config.canonical import canonical_json
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm
from uerl.tasks.cartpole import CARTPOLE_TASK_ID


def test_train_resume_resolves_source_and_writes_new_run_command(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    source = tmp_path / "source-run"
    source.mkdir()
    source_checkpoint = source / "model_final.pt"
    saved = training.build_run_config(
        CARTPOLE_TASK_ID,
        overrides={
            "worker.decimation": "[3,3]",
            "task.rew_scale_alive": "0.75",
        },
    )
    (source / "resolved_config.json").write_text(canonical_json(saved))
    terrain = TerrainLevelTerm(num_levels=1, num_envs=saved.worker.slot_count, terrain_size_x=30.0)
    torch.save(
        {
            "actor_state_dict": {"weight": torch.zeros(1)},
            "critic_state_dict": {"weight": torch.zeros(1)},
            "optimizer_state_dict": {"state": {}, "param_groups": [{"params": [0], "lr": 0.001}]},
            "iter": 4,
            "infos": {"uerl_curriculum": {"terrain": dict(terrain.state_dict())}},
        },
        source_checkpoint,
    )
    output = tmp_path / "new-run"
    captured = []

    def fake_run(config, *, terrain_level=None, freeze_observation_normalization=False):  # type: ignore[no-untyped-def]
        captured.append(config)
        return SimpleNamespace(
            task_id=config.task_id,
            iterations=1,
            checkpoint=output / "model_final.pt",
            metrics_directory=output / "rsl_rl",
            normalized_hash=config.normalized_hash,
        )

    monkeypatch.setattr(training, "run_training", fake_run)

    assert (
        train.main(
            [
                "--task",
                CARTPOLE_TASK_ID,
                "--session.mode",
                "attach",
                "--run-dir",
                str(output),
                "--resume",
                str(source),
                "--worker.decimation",
                "[3,3]",
                "--task.rew_scale_alive",
                "0.75",
                "--max-iterations",
                "7",
            ]
        )
        == 0
    )

    assert captured[0].logging.run_directory == output
    assert captured[0].runner.checkpoint == source_checkpoint
    assert captured[0].worker.decimation == (3, 3)
    assert captured[0].task.rew_scale_alive == 0.75
    assert captured[0].runner.max_iterations == 7
    assert (output / "command.txt").is_file()

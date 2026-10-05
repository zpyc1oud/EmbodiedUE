"""Verify Task-map defaults and explicit CLI precedence."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest
import torch

from uerl import training
from uerl.cli import play, train
from uerl.core.config.canonical import to_jsonable
from uerl.core.config.models import ResolvedRunConfig
from uerl.tasks.phantomx import PHANTOMX_TERRAIN_TASK_ID
from uerl.tasks.phantomx.evaluation import PhantomXEvaluationResult
from uerl.training import build_run_config


def test_train_map_flag_overrides_dotted_map_in_attach_mode(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    captured: list[ResolvedRunConfig] = []

    def run_training(
        config: ResolvedRunConfig, *, terrain_level: int | None = None
    ) -> SimpleNamespace:
        assert terrain_level is None
        captured.append(config)
        return SimpleNamespace(
            task_id=config.task_id,
            iterations=0,
            checkpoint=Path("model.pt"),
            metrics_directory=Path("metrics"),
            normalized_hash=config.normalized_hash,
        )

    monkeypatch.setattr(training, "run_training", run_training)

    result = train.main(
        [
            "--task",
            PHANTOMX_TERRAIN_TASK_ID,
            "--map",
            "/Game/Maps/Explicit",
            "--session.mode",
            "attach",
            "--session.map_path",
            "/Game/Maps/Dotted",
        ]
    )

    assert result == 0
    assert captured[0].session.map_path == "/Game/Maps/Explicit"


def test_play_map_flag_sets_expected_map_in_attach_mode(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    tmp_path: Path,
) -> None:
    captured: list[ResolvedRunConfig] = []

    def run_evaluation(
        config: ResolvedRunConfig,
        *,
        checkpoint: Path,
        steps: int,
        play_controller: str = "task",
        **kwargs: object,
    ) -> PhantomXEvaluationResult:
        del play_controller, kwargs
        captured.append(config)
        return PhantomXEvaluationResult(
            task_id=config.task_id,
            checkpoint=checkpoint,
            steps=steps,
                completed_episodes=0,
                mean_forward_velocity=0.0,
                mean_speed_error=0.0,
                success_rate=0.0,
                fall_rate=0.0,
            base_contact_rate=0.0,
            mean_episode_length=0.0,
            mean_command_vx=0.0,
            mean_command_speed=0.0,
            mean_actor_command_vx=0.0,
            mean_actor_command_speed=0.0,
            mean_command_observation_error=0.0,
            mean_action_clip_fraction=0.0,
            mean_torque_clip_fraction=0.0,
            mean_torque_over_limit=0.0,
            mean_reset_joint_error=0.0,
            mean_action_rate=0.0,
            mean_body_height=0.0,
            dominant_body_height_frequency_hz=0.0,
        )

    monkeypatch.setattr(training, "run_evaluation", run_evaluation)
    run = tmp_path / "run"
    run.mkdir()
    checkpoint = run / "model.pt"
    torch.save({"infos": {}}, checkpoint)
    from tests.python.run_config_files import write_resolved_config

    write_resolved_config(run, to_jsonable(build_run_config(PHANTOMX_TERRAIN_TASK_ID)))

    result = play.main(
        [
            "--task",
            PHANTOMX_TERRAIN_TASK_ID,
            "--checkpoint",
            str(checkpoint),
            "--map",
            "/Game/Maps/Explicit",
            "--session.mode",
            "attach",
        ]
    )

    assert result == 0
    assert captured[0].session.map_path == "/Game/Maps/Explicit"
    output = capsys.readouterr().out
    assert "terrain_level=auto" in output
    assert "forward_velocity=0.000000" in output
    assert "shutdown=PASS" in output

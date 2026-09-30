"""Verify Run checkpoint selection and derived deployment actuator metadata."""

from __future__ import annotations

from pathlib import Path

import pytest

from uerl import training
from uerl.cli import export, play
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.tasks.evaluation import EvaluationSummary


def test_play_run_selector_resolves_model_before_evaluation(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    run_directory = tmp_path / "run"
    run_directory.mkdir()
    checkpoint = run_directory / "model_final.pt"
    checkpoint.write_bytes(b"checkpoint")
    captured = []

    def fake_evaluation(config, *, checkpoint, steps, **kwargs):  # type: ignore[no-untyped-def]
        captured.append((config, checkpoint, steps, kwargs))
        return EvaluationSummary(
            task_id=config.task_id,
            checkpoint=checkpoint,
            steps=steps,
            completed_episodes=0,
            mean_episode_length=0.0,
            mean_reward=0.0,
        )

    monkeypatch.setattr(training, "run_evaluation", fake_evaluation)

    assert play.main(
        [
            "--task",
            CARTPOLE_TASK_ID,
            "--run",
            str(run_directory),
            "--steps",
            "3",
            "--session.mode",
            "attach",
        ]
    ) == 0

    assert captured[0][1] == checkpoint
    assert captured[0][2] == 3


def test_play_latest_uses_the_interrupted_checkpoint(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.chdir(tmp_path)
    run_directory = tmp_path / "runs" / CARTPOLE_TASK_ID / "20260923-150000-smoke"
    checkpoint = run_directory / "rsl_rl" / "model_50.pt"
    checkpoint.parent.mkdir(parents=True)
    checkpoint.write_bytes(b"checkpoint")
    (run_directory / "command.txt").write_text("train\n", encoding="utf-8")
    captured = []

    def fake_evaluation(config, *, checkpoint, steps, **kwargs):  # type: ignore[no-untyped-def]
        captured.append(checkpoint)
        return EvaluationSummary(
            task_id=config.task_id,
            checkpoint=checkpoint,
            steps=steps,
            completed_episodes=0,
            mean_episode_length=0.0,
            mean_reward=0.0,
        )

    monkeypatch.setattr(training, "run_evaluation", fake_evaluation)

    assert play.main(["--task", CARTPOLE_TASK_ID, "--run", "latest", "--session.mode", "attach"]) == 0

    assert captured[0].resolve() == checkpoint.resolve()


def test_resolved_robot_semantics_produce_artifact_runtime() -> None:
    config = training.build_run_config(CARTPOLE_TASK_ID)

    runtime = training.robot_runtime_from_config(config)

    assert len(runtime.actuators) == 1
    assert runtime.actuators[0].joint == "cart"
    assert runtime.actuators[0].effort_limit == 100.0
    assert runtime.actuators[0].default_position == 0.0


def test_export_rejects_ambiguous_or_missing_checkpoint_before_ue() -> None:
    with pytest.raises(SystemExit):
        export.main(
            [
                "--task",
                CARTPOLE_TASK_ID,
                "--run",
                "missing-run",
                "--checkpoint",
                "model.pt",
            ]
        )
    with pytest.raises(SystemExit):
        export.main(["--task", CARTPOLE_TASK_ID, "--checkpoint", "missing-model.pt"])

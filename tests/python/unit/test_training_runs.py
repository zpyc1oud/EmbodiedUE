"""Verify user-facing Run directories and explicit resume references."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
from unittest.mock import Mock

import pytest

from uerl import (
    DirectTaskConfig,
    LaunchMode,
    LoggingConfig,
    ResolvedRunConfig,
    RslRlRunnerConfig,
    SessionConfig,
    WorkerConfig,
)
from uerl.training.runner import run_training
from uerl.training.runs import (
    list_runs,
    make_run_directory,
    record_command,
    resolve_resume_checkpoint,
    resolve_run_directory,
)


def test_make_run_directory_scopes_task_and_resolves_same_second_collision(tmp_path: Path) -> None:
    now = datetime(2026, 9, 23, 14, 30, 0)

    first = make_run_directory("UERL-PhantomX-Walk-v0", root=tmp_path, run_name="walk", now=now)
    second = make_run_directory("UERL-PhantomX-Walk-v0", root=tmp_path, run_name="walk", now=now)

    assert first == tmp_path / "UERL-PhantomX-Walk-v0" / "20260923-143000-walk"
    assert second == tmp_path / "UERL-PhantomX-Walk-v0" / "20260923-143000-walk-01"
    assert first.is_dir()
    assert second.is_dir()


def test_resolve_resume_checkpoint_accepts_file_and_run_directory(tmp_path: Path) -> None:
    run_directory = tmp_path / "run"
    run_directory.mkdir()
    checkpoint = run_directory / "model_final.pt"
    checkpoint.write_bytes(b"checkpoint")

    assert resolve_resume_checkpoint(checkpoint) == checkpoint
    assert resolve_resume_checkpoint(run_directory) == checkpoint


def test_resolve_resume_checkpoint_rejects_missing_reference(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError, match="not found"):
        resolve_resume_checkpoint(tmp_path / "missing")

    run_directory = tmp_path / "empty-run"
    run_directory.mkdir()
    with pytest.raises(FileNotFoundError, match="no checkpoint"):
        resolve_resume_checkpoint(run_directory)


def test_resume_prefers_final_checkpoint_over_a_later_intermediate(tmp_path: Path) -> None:
    run_directory = tmp_path / "run"
    (run_directory / "rsl_rl").mkdir(parents=True)
    (run_directory / "rsl_rl" / "model_950.pt").write_bytes(b"intermediate")
    final = run_directory / "model_final.pt"
    final.write_bytes(b"final")

    assert resolve_resume_checkpoint(run_directory) == final


def test_interrupted_run_resumes_from_the_highest_iteration(tmp_path: Path) -> None:
    run_directory = tmp_path / "run"
    saved = run_directory / "rsl_rl"
    saved.mkdir(parents=True)
    for name in ("model_9.pt", "model_50.pt", "model_100.pt"):
        (saved / name).write_bytes(name.encode())

    assert resolve_resume_checkpoint(run_directory) == saved / "model_100.pt"


def test_latest_selects_the_newest_task_run(tmp_path: Path) -> None:
    task_id = "UERL-PhantomX-Walk-v0"
    older = tmp_path / task_id / "20260923-140000-walk"
    newer = tmp_path / task_id / "20260923-150000-walk"
    for run_directory, name in ((older, "model_50.pt"), (newer, "model_100.pt")):
        (run_directory / "rsl_rl").mkdir(parents=True)
        (run_directory / "rsl_rl" / name).write_bytes(b"checkpoint")
        (run_directory / "command.txt").write_text("train\n", encoding="utf-8")
    other = tmp_path / "UERL-CartPole-Direct-v0" / "20260923-160000-smoke"
    other.mkdir(parents=True)
    (other / "command.txt").write_text("train\n", encoding="utf-8")

    assert resolve_run_directory("latest", task_id=task_id, root=tmp_path) == newer
    assert resolve_resume_checkpoint("latest", task_id=task_id, root=tmp_path) == newer / "rsl_rl" / "model_100.pt"
    listed = list_runs(task_id=task_id, root=tmp_path)
    assert [item.directory for item in listed] == [newer, older]


def test_record_command_writes_one_replayable_line(tmp_path: Path) -> None:
    command_path = record_command(tmp_path / "run", ["uerl", "train", "--task", "task", "--run-name", "walk"])

    assert command_path == tmp_path / "run" / "command.txt"
    assert command_path.read_text(encoding="utf-8") == 'uerl train --task task --run-name walk\n'


def test_training_does_not_load_existing_output_without_explicit_resume(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    run_directory = tmp_path / "run"
    run_directory.mkdir()
    (run_directory / "model_final.pt").write_bytes(b"old output")
    config = ResolvedRunConfig(
        task_id="task",
        task_version="1.0",
        session=SessionConfig(mode=LaunchMode.ATTACH),
        worker=WorkerConfig(environment_id="environment", robot_id="robot"),
        task=DirectTaskConfig(),
        runner=RslRlRunnerConfig(rollout_length=4, max_iterations=1, checkpoint=None),
        logging=LoggingConfig(run_directory=run_directory),
        normalized_hash="a" * 64,
    )
    raw_session = Mock()
    registry = Mock()
    registry.create_task.return_value = Mock()
    vec_env = Mock()
    order: list[str] = []

    class _Runner:
        def __init__(self, _env: object, _cfg: object, *, log_dir: str, device: str) -> None:
            del log_dir, device

        def learn(self, _iterations: int, *, init_at_random_ep_len: bool = False) -> None:
            del init_at_random_ep_len
            order.append("learn")

        def save(self, path: str) -> None:
            order.append("save")
            Path(path).write_bytes(b"new output")

    monkeypatch.setattr("uerl.training.runner.create_default_registry", lambda: registry)
    monkeypatch.setattr("uerl.training.runner.UERLSession.open", Mock(return_value=raw_session))
    monkeypatch.setattr("uerl.training.runner.UERLSessionAdapter", lambda *_args, **_kwargs: Mock())
    monkeypatch.setattr("uerl.training.runner.UERLDirectEnv", Mock())
    monkeypatch.setattr("uerl.training.runner.UERLVecEnvWrapper", Mock(return_value=vec_env))
    monkeypatch.setattr("uerl.training.runner.UERLOnPolicyRunner", _Runner)

    result = run_training(config)

    assert order == ["learn", "save"]
    assert result.checkpoint == run_directory / "model_final.pt"
    assert (run_directory / "model_final.pt").read_bytes() == b"new output"
    vec_env.close.assert_called_once_with("training_complete")

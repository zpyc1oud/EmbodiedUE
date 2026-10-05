"""Check saved-run CLI configuration at the Session boundary without launching UE."""

from __future__ import annotations

import json
from pathlib import Path
from typing import NoReturn

import pytest
import torch

from tests.python.run_config_files import write_resolved_config
from uerl.cli import export, play
from uerl.core.config import ResolvedRunConfig
from uerl.core.config.canonical import to_jsonable
from uerl.core.config.snapshot import CHECKPOINT_CONFIG_KEY, resolved_config_to_yaml
from uerl.runtime.session import UERLSession
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


def _write_checkpoint(path: Path) -> None:
    torch.save({"infos": {}}, path)


@pytest.mark.parametrize("command", ["play", "export"])
@pytest.mark.parametrize("mode", ["attach", "launch"])
@pytest.mark.parametrize("map_source", ["recorded", "dotted", "flag"])
def test_saved_configuration_reaches_session_after_launch_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, command: str, mode: str, map_source: str,
) -> None:
    run = tmp_path / "run"
    run.mkdir()
    _write_checkpoint(run / "model_final.pt")
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    payload["session"]["map_path"] = "/Game/Maps/Recorded"
    payload["worker"]["decimation"] = [3, 3]
    payload["task"]["rew_scale_alive"] = 0.75
    payload["runner"]["parameters"]["hidden_dims"] = [64, 64]
    snapshot = write_resolved_config(run, payload)
    before = snapshot.read_bytes()
    captured: list[ResolvedRunConfig] = []

    class SessionBoundaryReached(Exception):
        pass

    def capture(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        captured.append(config)
        raise SessionBoundaryReached

    monkeypatch.setattr(UERLSession, "open", capture)
    argv = ["--task", CARTPOLE_TASK_ID, "--run", str(run), "--session.mode", mode]
    expected_map = "/Game/Maps/Recorded"
    if map_source in ("dotted", "flag"):
        argv += ["--session.map_path", "/Game/Maps/Dotted"]
        expected_map = "/Game/Maps/Dotted"
    if map_source == "flag":
        argv += ["--map", "/Game/Maps/Flag"]
        expected_map = "/Game/Maps/Flag"

    with pytest.raises(SessionBoundaryReached):
        (play.main if command == "play" else export.main)(argv)

    config, = captured
    assert config.session.map_path == expected_map
    if mode == "launch":
        assert expected_map in config.session.worker_args
    assert config.worker.decimation == (3, 3)
    assert to_jsonable(config.task)["rew_scale_alive"] == 0.75
    assert config.runner.parameters["hidden_dims"] == (64, 64)
    assert snapshot.read_bytes() == before


@pytest.mark.parametrize("command", ["play", "export"])
@pytest.mark.parametrize("checkpoint_source", ["run", "checkpoint"])
def test_run_path_infers_task_and_checkpoint_identity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
    checkpoint_source: str,
) -> None:
    run = tmp_path / "run"
    run.mkdir()
    checkpoint = run / "model_final.pt"
    _write_checkpoint(checkpoint)
    checkpoint_before = checkpoint.read_bytes()
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    snapshot = write_resolved_config(run, payload)
    before = snapshot.read_bytes()
    captured: list[ResolvedRunConfig] = []

    class SessionBoundaryReached(Exception):
        pass

    def capture(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        captured.append(config)
        raise SessionBoundaryReached

    monkeypatch.setattr(UERLSession, "open", capture)
    if checkpoint_source == "run":
        argv = ["--run", str(run), "--session.mode", "attach"]
    else:
        argv = ["--checkpoint", str(checkpoint), "--session.mode", "attach"]

    with pytest.raises(SessionBoundaryReached):
        (play.main if command == "play" else export.main)(argv)

    config, = captured
    assert config.task_id == CARTPOLE_TASK_ID
    assert snapshot.read_bytes() == before
    assert checkpoint.read_bytes() == checkpoint_before


@pytest.mark.parametrize("command", ["play", "export"])
def test_detached_checkpoint_infers_task_and_saved_settings_from_embedded_yaml(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
) -> None:
    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={"worker.decimation": "[3, 3]", "task.rew_scale_alive": "0.75"},
    )
    checkpoint = tmp_path / "detached.pt"
    torch.save({"infos": {CHECKPOINT_CONFIG_KEY: resolved_config_to_yaml(config)}}, checkpoint)
    before = checkpoint.read_bytes()
    captured: list[ResolvedRunConfig] = []

    class SessionBoundaryReached(Exception):
        pass

    def capture(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        captured.append(config)
        raise SessionBoundaryReached

    monkeypatch.setattr(UERLSession, "open", capture)
    argv = ["--checkpoint", str(checkpoint), "--session.mode", "attach"]
    if command == "export":
        argv.extend(("--output", str(tmp_path / "policy.uerlpol2")))

    with pytest.raises(SessionBoundaryReached):
        (play.main if command == "play" else export.main)(argv)

    (restored,) = captured
    assert restored.task_id == CARTPOLE_TASK_ID
    assert restored.worker.decimation == (3, 3)
    assert to_jsonable(restored.task)["rew_scale_alive"] == 0.75
    assert checkpoint.read_bytes() == before


@pytest.mark.parametrize("command", ["play", "export"])
def test_explicit_task_conflict_fails_before_session(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
) -> None:
    run = tmp_path / "run"
    run.mkdir()
    _write_checkpoint(run / "model_final.pt")
    write_resolved_config(run, to_jsonable(build_run_config(CARTPOLE_TASK_ID)))
    calls: list[ResolvedRunConfig] = []

    def capture(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        calls.append(config)
        raise AssertionError("a conflicting Task must fail before Session.open")

    monkeypatch.setattr(UERLSession, "open", capture)

    result = (play.main if command == "play" else export.main)(
        ["--task", "UERL-PhantomX-Walk-v0", "--run", str(run), "--session.mode", "attach"]
    )

    assert result == 1
    assert calls == []


@pytest.mark.parametrize("command", ["play", "export"])
def test_missing_run_config_fails_before_session_with_recovery_guidance(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    command: str,
) -> None:
    run = tmp_path / "old-run"
    run.mkdir()
    _write_checkpoint(run / "model_final.pt")
    calls: list[ResolvedRunConfig] = []

    def capture(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        calls.append(config)
        raise AssertionError("missing metadata must fail before Session.open")

    monkeypatch.setattr(UERLSession, "open", capture)

    result = (play.main if command == "play" else export.main)(
        ["--run", str(run), "--session.mode", "attach"]
    )

    assert result == 1
    assert calls == []
    assert "restore it from the original Run" in capsys.readouterr().out


@pytest.mark.parametrize("command", ["play", "export"])
def test_checkpoint_inside_historical_json_run_gets_migration_guidance(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    command: str,
) -> None:
    run = tmp_path / "old-run"
    run.mkdir()
    checkpoint = run / "model_final.pt"
    _write_checkpoint(checkpoint)
    (run / "resolved_config.json").write_text(
        json.dumps(to_jsonable(build_run_config(CARTPOLE_TASK_ID))),
        encoding="utf-8",
    )

    result = (play.main if command == "play" else export.main)(
        ["--checkpoint", str(checkpoint), "--session.mode", "attach"]
    )

    assert result == 1
    assert "run_migration" in capsys.readouterr().out


@pytest.mark.parametrize("command", ["play", "export"])
def test_missing_saved_session_protocol_fails_before_session(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    command: str,
) -> None:
    run = tmp_path / "run"
    run.mkdir()
    _write_checkpoint(run / "model_final.pt")
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    del payload["session"]["protocol"]
    snapshot = write_resolved_config(run, payload)
    before = snapshot.read_bytes()

    def unexpected_session_open(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        raise AssertionError("missing protocol must fail before Session.open")

    monkeypatch.setattr(UERLSession, "open", unexpected_session_open)

    result = (play.main if command == "play" else export.main)(
        ["--run", str(run), "--session.mode", "attach"]
    )

    assert result == 1
    assert "--session.protocol" in capsys.readouterr().out
    assert snapshot.read_bytes() == before


@pytest.mark.parametrize("command", ["play", "export"])
def test_latest_requires_task_before_file_resolution_or_session_open(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    command: str,
) -> None:
    from uerl import training

    monkeypatch.chdir(tmp_path)

    def unexpected_file_resolution(*args: object, **kwargs: object) -> NoReturn:
        raise AssertionError("latest without a Task must fail before Run lookup")

    def unexpected_session_open(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        raise AssertionError("latest without a Task must fail before Session.open")

    monkeypatch.setattr(training, "resolve_run_directory", unexpected_file_resolution)
    monkeypatch.setattr(training, "resolve_resume_checkpoint", unexpected_file_resolution)
    monkeypatch.setattr(UERLSession, "open", unexpected_session_open)

    with pytest.raises(SystemExit) as error:
        (play.main if command == "play" else export.main)(["--run", "latest"])

    assert error.value.code == 2
    assert "--task is required with --run latest" in capsys.readouterr().err
    assert not (tmp_path / "runs").exists()


@pytest.mark.parametrize("command", ["play", "export"])
def test_checkpoint_rejects_task_from_unrelated_ancestor_snapshot(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    command: str,
) -> None:
    run = tmp_path / "unrelated-run"
    checkpoint = run / "rsl_rl" / "model_10.pt"
    checkpoint.parent.mkdir(parents=True)
    _write_checkpoint(checkpoint)
    checkpoint_before = checkpoint.read_bytes()
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    snapshot = write_resolved_config(run, payload)
    snapshot_before = snapshot.read_bytes()

    def unexpected_session_open(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        raise AssertionError("an explicit Task conflict must fail before Session.open")

    monkeypatch.setattr(UERLSession, "open", unexpected_session_open)

    result = (play.main if command == "play" else export.main)(
        [
            "--task",
            "UERL-PhantomX-Walk-v0",
            "--checkpoint",
            str(checkpoint),
            "--session.mode",
            "attach",
        ]
    )

    assert result == 1
    output = capsys.readouterr().out
    assert "Run was trained for 'UERL-CartPole-Direct-v0'" in output
    assert "not 'UERL-PhantomX-Walk-v0'" in output
    assert snapshot.read_bytes() == snapshot_before
    assert checkpoint.read_bytes() == checkpoint_before


@pytest.mark.parametrize("command", ["play", "export"])
def test_semantic_override_cannot_change_a_saved_run(
    tmp_path: Path,
    command: str,
) -> None:
    run = tmp_path / "run"
    run.mkdir()
    _write_checkpoint(run / "model_final.pt")
    write_resolved_config(run, to_jsonable(build_run_config(CARTPOLE_TASK_ID)))

    with pytest.raises(SystemExit):
        (play.main if command == "play" else export.main)(
            ["--run", str(run), "--worker.decimation", "3,3", "--session.mode", "attach"]
        )


@pytest.mark.parametrize("command", ["play", "export"])
def test_saved_run_commands_reject_multiple_slots(
    tmp_path: Path,
    command: str,
) -> None:
    run = tmp_path / "run"
    run.mkdir()
    _write_checkpoint(run / "model_final.pt")
    write_resolved_config(run, to_jsonable(build_run_config(CARTPOLE_TASK_ID)))

    with pytest.raises(SystemExit):
        (play.main if command == "play" else export.main)(
            ["--run", str(run), "--worker.slot_count", "2", "--session.mode", "attach"]
        )

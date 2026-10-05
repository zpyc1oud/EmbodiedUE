"""Machine profiles reach each command's Session boundary without running UE."""

from __future__ import annotations

import json
from pathlib import Path
from typing import NoReturn

import pytest

from uerl.cli import export, play, train
from uerl.core.config import ResolvedRunConfig
from uerl.runtime.session import UERLSession
from uerl.tasks.cartpole import CARTPOLE_TASK_ID

COMMANDS = {"train": train.main, "play": play.main, "export": export.main}


class SessionBoundaryReached(Exception):
    pass


def command_args(command: str, root: Path) -> list[str]:
    checkpoint = root / "model.pt"
    checkpoint.write_bytes(b"not loaded before Session.open")
    args = ["--task", CARTPOLE_TASK_ID, "--device", "cpu", "--session.port", "44555"]
    if command == "train":
        return args + ["--run-dir", str(root / "output"), "--num-envs", "1"]
    from uerl.core.config.canonical import to_jsonable
    from uerl.training import build_run_config

    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    payload["worker"]["decimation"] = [3, 3]
    payload["task"]["rew_scale_alive"] = 0.75
    payload["runner"]["parameters"]["hidden_dims"] = [64, 64]
    (root / "resolved_config.json").write_text(json.dumps(payload), encoding="utf-8")
    args += ["--checkpoint", str(checkpoint)]
    if command == "export":
        args += ["--output", str(root / "output.uerlpol2")]
    return args


def capture_session(monkeypatch: pytest.MonkeyPatch) -> list[ResolvedRunConfig]:
    captured: list[ResolvedRunConfig] = []

    def capture(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        captured.append(config)
        raise SessionBoundaryReached

    monkeypatch.setattr(UERLSession, "open", capture)
    return captured


@pytest.mark.parametrize("command", COMMANDS)
def test_runtime_commands_use_environment_profile(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
) -> None:
    profile = tmp_path / "host.toml"
    profile.write_text("ue_executable = 'UE/editor.exe'\nproject = 'host/project.uproject'\n", encoding="utf-8")
    monkeypatch.setenv("UERL_HOST_PROFILE", str(profile))
    captured = capture_session(monkeypatch)
    with pytest.raises(SessionBoundaryReached):
        COMMANDS[command](command_args(command, tmp_path))
    (config,) = captured
    assert config.session.worker_executable == tmp_path / "UE/editor.exe"
    assert config.session.worker_args[0] == str(tmp_path / "host/project.uproject")
    assert "-uerlport=44555" in config.session.worker_args


@pytest.mark.parametrize("command", COMMANDS)
@pytest.mark.parametrize("selector", ["default", "environment", "explicit"])
@pytest.mark.parametrize("explicit_paths", ["none", "executable", "project", "both"])
def test_profile_and_path_precedence_reaches_session(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
    selector: str,
    explicit_paths: str,
) -> None:
    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.delenv("UERL_HOST_PROFILE", raising=False)
    profiles = {
        "default": tmp_path / ".uerl/host.toml",
        "environment": tmp_path / "environment.toml",
        "explicit": tmp_path / "explicit.toml",
    }
    for name, path in profiles.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"ue_executable = '{name}.exe'\nproject = '{name}.uproject'", encoding="utf-8")
    args = command_args(command, tmp_path)
    if selector != "default":
        monkeypatch.setenv("UERL_HOST_PROFILE", str(profiles["environment"]))
    if selector == "explicit":
        args += ["--host-profile", str(profiles["explicit"])]
    expected_executable = profiles[selector].parent / f"{selector}.exe"
    expected_project = profiles[selector].parent / f"{selector}.uproject"
    # Relative flags resolve from cwd, not from the selected profile's folder.
    if explicit_paths in ("executable", "both"):
        expected_executable = Path("flag.exe").resolve()
        args += ["--ue-executable", "flag.exe"]
    if explicit_paths in ("project", "both"):
        expected_project = Path("flag.uproject").resolve()
        args += ["--project", "flag.uproject"]
    captured = capture_session(monkeypatch)
    with pytest.raises(SessionBoundaryReached):
        COMMANDS[command](args)
    (config,) = captured
    assert config.session.worker_executable == expected_executable
    assert config.session.worker_args[0] == str(expected_project)
    assert config.session.worker_args[1] == "/Engine/Maps/Entry"
    assert config.session.port == 44555


@pytest.mark.parametrize("command", COMMANDS)
def test_dotted_launch_overrides_still_win_over_profile_and_flags(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
) -> None:
    profile = tmp_path / "host.toml"
    profile.write_text("ue_executable = 'profile.exe'\nproject = 'profile.uproject'", encoding="utf-8")
    captured = capture_session(monkeypatch)
    args = command_args(command, tmp_path) + [
        "--host-profile",
        str(profile),
        "--ue-executable",
        "flag.exe",
        "--project",
        "flag.uproject",
        "--session.worker_executable",
        "dotted.exe",
        "--session.worker_args",
        '["dotted.uproject", "-custom"]',
    ]
    with pytest.raises(SessionBoundaryReached):
        COMMANDS[command](args)
    (config,) = captured
    assert config.session.worker_executable == Path("dotted.exe")
    assert config.session.worker_args == ("dotted.uproject", "-custom")


@pytest.mark.parametrize("command", COMMANDS)
@pytest.mark.parametrize("bad_profile", ["missing", "malformed"])
def test_attach_ignores_host_profile_and_never_launches_a_worker(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
    bad_profile: str,
) -> None:
    from uerl.core.config import LaunchMode, SessionConfig
    from uerl.runtime.session import WorkerOwnership, WorkerProcessController

    profile = tmp_path / "host.toml"
    if bad_profile == "malformed":
        profile.write_text("invalid = [", encoding="utf-8")
    monkeypatch.setenv("UERL_HOST_PROFILE", str(profile))
    real_open = UERLSession.open
    captured: list[ResolvedRunConfig] = []

    def forbidden_process(*args: object, **kwargs: object) -> NoReturn:
        pytest.fail("attach must not start a Worker process")

    controller = WorkerProcessController(popen_factory=forbidden_process)

    def stop_before_connect(config: SessionConfig) -> NoReturn:
        assert config.mode is LaunchMode.ATTACH
        assert controller.handle is not None
        assert controller.handle.ownership is WorkerOwnership.ATTACHED
        assert controller.handle.process is None
        raise SessionBoundaryReached

    def open_at_bridge(config: ResolvedRunConfig, **kwargs: object) -> UERLSession:
        captured.append(config)
        return real_open(config, process_controller=controller, bridge_factory=stop_before_connect)

    monkeypatch.setattr(UERLSession, "open", open_at_bridge)
    args = command_args(command, tmp_path) + [
        "--session.mode",
        "attach",
        "--host-profile",
        str(profile),
        "--ue-executable",
        "ignored.exe",
        "--project",
        "ignored.uproject",
    ]
    with pytest.raises(SessionBoundaryReached):
        COMMANDS[command](args)
    (config,) = captured
    assert config.session.worker_executable is None
    assert config.session.worker_args == ()
    assert config.session.port == 44555


@pytest.mark.parametrize("command", COMMANDS)
def test_invalid_launch_profile_fails_before_session(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
    capsys: pytest.CaptureFixture[str],
) -> None:
    monkeypatch.setenv("UERL_HOST_PROFILE", str(tmp_path / "missing.toml"))
    captured = capture_session(monkeypatch)
    assert COMMANDS[command](command_args(command, tmp_path)) == 1
    assert captured == []
    assert "Cannot read host profile" in capsys.readouterr().out


@pytest.mark.parametrize("command", ["play", "export"])
def test_machine_paths_do_not_replace_saved_task_semantics(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
    capsys: pytest.CaptureFixture[str],
) -> None:
    import json

    from uerl.core.config.canonical import to_jsonable
    from uerl.training import build_run_config

    source = tmp_path / "saved-run"
    source.mkdir()
    (source / "model_final.pt").write_bytes(b"not loaded before Session.open")
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    payload["worker"]["decimation"] = [3, 3]
    payload["task"]["rew_scale_alive"] = 0.75
    payload["runner"]["parameters"]["hidden_dims"] = [64, 64]
    payload["session"]["worker_executable"] = "old-machine.exe"
    payload["session"]["worker_args"] = ["old-machine.uproject"]
    snapshot = source / "resolved_config.json"
    snapshot.write_text(json.dumps(payload), encoding="utf-8")
    before = snapshot.read_bytes()
    profile = tmp_path / "host.toml"
    profile.write_text("ue_executable = 'new-machine.exe'\nproject = 'new-machine.uproject'", encoding="utf-8")
    monkeypatch.setenv("UERL_HOST_PROFILE", str(profile))
    captured = capture_session(monkeypatch)
    with pytest.raises(SessionBoundaryReached):
        COMMANDS[command](["--task", CARTPOLE_TASK_ID, "--run", str(source), "--device", "cpu"])
    (config,) = captured
    assert config.session.worker_executable == tmp_path / "new-machine.exe"
    assert config.session.worker_args[0] == str(tmp_path / "new-machine.uproject")
    assert config.worker.decimation == (3, 3)
    assert to_jsonable(config.task)["rew_scale_alive"] == 0.75
    assert config.runner.parameters["hidden_dims"] == (64, 64)
    assert snapshot.read_bytes() == before
    if command == "export":
        assert f"run_hash={payload['normalized_hash']}" in capsys.readouterr().out


@pytest.mark.parametrize("command", COMMANDS)
def test_missing_implicit_profile_keeps_existing_defaults(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
) -> None:
    from uerl.host.profile import DEFAULT_PROJECT, DEFAULT_UE_EXECUTABLE

    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.delenv("UERL_HOST_PROFILE", raising=False)
    captured = capture_session(monkeypatch)
    with pytest.raises(SessionBoundaryReached):
        COMMANDS[command](command_args(command, tmp_path))
    (config,) = captured
    assert config.session.worker_executable == DEFAULT_UE_EXECUTABLE
    assert config.session.worker_args[0] == str(DEFAULT_PROJECT)


@pytest.mark.parametrize("command", COMMANDS)
def test_changing_host_profile_changes_only_session_settings(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    command: str,
) -> None:
    from uerl.core.config.canonical import to_jsonable

    captured = capture_session(monkeypatch)
    for machine in ("first", "second"):
        profile = tmp_path / f"{machine}.toml"
        profile.write_text(f"ue_executable = '{machine}.exe'\nproject = '{machine}.uproject'", encoding="utf-8")
        args = command_args(command, tmp_path) + [
            "--host-profile",
            str(profile),
        ]
        if command == "train":
            args += ["--worker.decimation", "[3,3]", "--task.rew_scale_alive", "0.75"]
        with pytest.raises(SessionBoundaryReached):
            COMMANDS[command](args)
    first, second = captured
    assert first.session.worker_executable != second.session.worker_executable
    assert first.session.worker_args[0] != second.session.worker_args[0]
    assert first.worker.decimation == second.worker.decimation == (3, 3)
    assert to_jsonable(first.task)["rew_scale_alive"] == to_jsonable(second.task)["rew_scale_alive"] == 0.75
    first_payload, second_payload = to_jsonable(first), to_jsonable(second)
    for payload in (first_payload, second_payload):
        del payload["session"]
        del payload["normalized_hash"]
    assert first_payload == second_payload

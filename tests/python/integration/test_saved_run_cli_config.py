"""Check saved-run CLI configuration at the Session boundary without launching UE."""

from __future__ import annotations

import json
from pathlib import Path
from typing import NoReturn

import pytest

from uerl.cli import export, play
from uerl.core.config import ResolvedRunConfig
from uerl.core.config.canonical import to_jsonable
from uerl.runtime.session import UERLSession
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


@pytest.mark.parametrize("command", ["play", "export"])
@pytest.mark.parametrize("mode", ["attach", "launch"])
@pytest.mark.parametrize("map_source", ["recorded", "dotted", "flag"])
def test_saved_configuration_reaches_session_after_launch_overrides(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, command: str, mode: str, map_source: str,
) -> None:
    run = tmp_path / "run"
    run.mkdir()
    (run / "model_final.pt").write_bytes(b"not loaded before Session.open")
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    payload["session"]["map_path"] = "/Game/Maps/Recorded"
    payload["worker"]["decimation"] = [3, 3]
    payload["task"]["rew_scale_alive"] = 0.75
    payload["runner"]["parameters"]["hidden_dims"] = [64, 64]
    snapshot = run / "resolved_config.json"
    snapshot.write_text(json.dumps(payload), encoding="utf-8")
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

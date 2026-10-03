"""Exercise both training configuration builds up to the real Session seam."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from typing import NoReturn

import pytest
import torch

from uerl.cli import train
from uerl.core.config import ResolvedRunConfig
from uerl.core.config.canonical import canonical_json, to_jsonable
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm
from uerl.runtime.session import UERLSession
from uerl.tasks.cartpole import CARTPOLE_TASK_ID, registration
from uerl.tasks.cartpole.config import load_cartpole_training_config
from uerl.training import build_run_config


@pytest.mark.parametrize("reference", ["run", "file", "latest", "dotted"])
@pytest.mark.parametrize("mode", ["attach", "launch"])
def test_resume_preserves_semantics_at_final_session_and_reports_exact_budget_diff(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    reference: str,
    mode: str,
) -> None:
    run = tmp_path / "runs" / CARTPOLE_TASK_ID / "20261003-source"
    run.mkdir(parents=True)
    checkpoint = run / "model_final.pt"
    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={
            "worker.slot_count": "2",
            "worker.decimation": "[3,3]",
            "task.rew_scale_alive": "0.75",
            "runner.parameters.hidden_dims": "[8,8]",
            "runner.device": "cpu",
            "runner.max_iterations": "10",
            "session.map_path": "/Game/Maps/Recorded",
            "logging.run_directory": str(run),
        },
    )
    (run / "resolved_config.json").write_text(canonical_json(config))
    term = TerrainLevelTerm(num_levels=1, num_envs=2, terrain_size_x=30.0)
    torch.save(
        {
            "actor_state_dict": {"weight": torch.zeros(1)},
            "critic_state_dict": {"weight": torch.zeros(1)},
            "optimizer_state_dict": {"state": {}, "param_groups": [{"params": [0], "lr": 0.001}]},
            "iter": 12,
            "infos": {"uerl_curriculum": {"terrain": dict(term.state_dict())}},
        },
        checkpoint,
    )
    before = {path.name: path.read_bytes() for path in run.iterdir()}
    original = load_cartpole_training_config()
    changed = replace(
        original, task=replace(original.task, rew_scale_alive=9.0), worker=replace(original.worker, decimation=(5, 5))
    )
    monkeypatch.setattr(registration, "load_cartpole_training_config", lambda: changed)
    monkeypatch.chdir(tmp_path)
    captured: list[ResolvedRunConfig] = []

    class SessionReached(Exception):
        pass

    def capture(config: ResolvedRunConfig, **kwargs: object) -> NoReturn:
        captured.append(config)
        raise SessionReached

    monkeypatch.setattr(UERLSession, "open", capture)
    selected = {"run": str(run), "file": str(checkpoint), "latest": "latest", "dotted": str(checkpoint)}[reference]
    argv = [
        "--task",
        CARTPOLE_TASK_ID,
        "--runner.checkpoint" if reference == "dotted" else "--resume",
        selected,
        "--session.mode",
        mode,
        "--run-dir",
        str(tmp_path / "new-run"),
        "--max-iterations",
        "7",
    ]
    with pytest.raises(SessionReached):
        train.main(argv)

    (final,) = captured
    assert final.worker.decimation == (3, 3)
    assert to_jsonable(final.task)["rew_scale_alive"] == 0.75
    assert final.runner.parameters["hidden_dims"] == (8, 8)
    assert final.runner.max_iterations == 7
    assert final.runner.checkpoint == checkpoint
    assert final.session.map_path == "/Game/Maps/Recorded"
    if mode == "launch":
        assert "/Game/Maps/Recorded" in final.session.worker_args
    output = capsys.readouterr().out
    assert "[CONFIG] runner.max_iterations: 10 -> 7 source=explicit override" in output
    assert "[CONFIG] task." not in output
    assert "[CONFIG] worker." not in output
    assert "iteration=12" in output
    assert before == {path.name: path.read_bytes() for path in run.iterdir()}


@pytest.mark.parametrize("case", ["missing_config", "reward_change", "map_change", "missing_optimizer"])
def test_invalid_continuation_stops_before_session_or_output(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    case: str,
) -> None:
    run = tmp_path / "source"
    run.mkdir()
    output = tmp_path / "new-run"
    config = build_run_config(CARTPOLE_TASK_ID)
    if case != "missing_config":
        (run / "resolved_config.json").write_text(canonical_json(config))
    payload: dict[str, object] = {
        "actor_state_dict": {"weight": torch.zeros(1)},
        "critic_state_dict": {"weight": torch.zeros(1)},
        "optimizer_state_dict": {"state": {}, "param_groups": [{"params": [0], "lr": 0.001}]},
        "iter": 1,
    }
    if case == "missing_optimizer":
        del payload["optimizer_state_dict"]
    torch.save(payload, run / "model_final.pt")
    before = {path.name: path.read_bytes() for path in run.iterdir()}

    def forbidden(*args: object, **kwargs: object) -> NoReturn:
        pytest.fail("invalid continuation reached Session.open")

    monkeypatch.setattr(UERLSession, "open", forbidden)
    argv = ["--task", CARTPOLE_TASK_ID, "--resume", str(run), "--run-dir", str(output)]
    if case == "reward_change":
        argv += ["--task.rew_scale_alive", "2"]
    if case == "map_change":
        argv += ["--map", "/Game/Maps/Other"]
    assert train.main(argv) == 1
    assert not output.exists()
    assert before == {path.name: path.read_bytes() for path in run.iterdir()}

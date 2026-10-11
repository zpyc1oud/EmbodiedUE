"""Train, restore/export and infer the same declared-input policy in a game host."""
from __future__ import annotations

import json
import sys
from pathlib import Path

from scripts.test_runner_support import TestRun
from tests.e2e.support.worker_runner import UE_CMD, UPROJECT, run_owned_command
from uerl.policy.artifact import PolicyArtifact

REPO = Path(__file__).resolve().parents[2]
GAME_CASE = "UERL.E2E.Policy.FreshExport"


def test_fresh_declared_input_policy_trains_exports_and_runs_in_game(tmp_path: Path) -> None:
    run = tmp_path / "run"
    artifact = tmp_path / "fresh.uerlpol2"
    fields = [f"robot.joint.{joint}.joint_{kind}" for joint in ("pole", "cart") for kind in ("position", "velocity")]
    inputs = json.dumps([{
        "name": "state", "provider_id": "uerl.state", "provider_version": 1,
        "parameters": {"fields": fields},
    }])
    host = ["--ue-executable", UE_CMD, "--project", UPROJECT]
    train = run_owned_command([
        sys.executable, "-m", "uerl.cli.train", *host, "--task", "UERL-CartPole-Direct-v0",
        "--num-envs", "2", "--max-iterations", "1", "--device", "cpu",
        "--run-dir", str(run), "--worker.inputs", inputs,
    ], cwd=REPO, timeout=480)
    assert train.returncode == 0, train.stdout + train.stderr
    assert (run / "model_final.pt").is_file()
    export = run_owned_command([
        sys.executable, "-m", "uerl.cli.export", *host, "--run", str(run), "--output", str(artifact),
    ], cwd=REPO, timeout=480)
    assert export.returncode == 0, export.stdout + export.stderr
    policy = PolicyArtifact.read(artifact)
    assert policy.format_version == 2
    assert len(policy.input_requirements) == 1
    assert [f.name for f in policy.input_requirements[0].fields] == fields
    validation = TestRun(tmp_path / "game", ["fresh policy game inference"], 180)
    log = validation.directory / "game.log"
    assert validation.run(0, [
        UE_CMD, UPROJECT, "/Engine/Maps/Entry", "-unattended", "-nullrhi", "-nosound", "-NoSplash",
        f"-uerlinputartifact={artifact}", f"-abslog={log}",
        f"-ExecCmds=Automation RunTests {GAME_CASE};Quit",
    ]) == 0
    assert validation.check_automation(0, log, 1, required_names=(GAME_CASE,)) == 0

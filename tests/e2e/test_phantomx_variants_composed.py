"""Ticket 28: four PhantomX DirectTask variants — short E2E train + walk export."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from tests.e2e.support.worker_runner import UE_CMD, UPROJECT
from uerl.policy.artifact import PolicyArtifact
from uerl.tasks.phantomx.config import (
    PHANTOMX_DISCRETE_TERRAIN_TASK_ID,
    PHANTOMX_JOINTS,
    PHANTOMX_PURSUIT_TASK_ID,
    PHANTOMX_TASK_ID,
    PHANTOMX_TERRAIN_TASK_ID,
)
from uerl.tasks.phantomx.observation_plan import PHANTOMX_JOINT_DEFAULTS

REPO_ROOT = Path(__file__).resolve().parents[2]

# Convergence criterion (ticket 28): smoke gate — each variant completes ≥1
# training iteration and writes model_final.pt. Full-curve parity vs pre-migration
# baselines (e.g. last-30-iter mean return within 10%) is a longer regression job,
# not required to close this ticket once the smoke gate is green.
_VARIANT_TASKS = (
    PHANTOMX_TASK_ID,
    PHANTOMX_PURSUIT_TASK_ID,
    PHANTOMX_TERRAIN_TASK_ID,
    PHANTOMX_DISCRETE_TERRAIN_TASK_ID,
)


def _write_robot_runtime(path: Path) -> None:
    payload = {
        "actuators": [
            {
                "joint": joint,
                "stiffness": 25.0,
                "damping": 0.5,
                "effort_limit": 2.8,
                "default_position": float(PHANTOMX_JOINT_DEFAULTS[index]),
            }
            for index, joint in enumerate(PHANTOMX_JOINTS)
        ]
    }
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


@pytest.mark.parametrize("task_id", _VARIANT_TASKS)
def test_phantomx_composed_variant_trains_one_iteration(task_id: str, tmp_path: Path) -> None:
    """Each registered DirectTask variant completes a one-iteration headless train."""

    run_directory = tmp_path / task_id.replace(":", "_")
    # The ticket11 contract exercises the new [1,7] timing interval.  Keep the
    # smoke test single-slot so it validates the task wiring without turning a
    # one-iteration check into a capacity benchmark.
    slot_count = "1"
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "uerl.cli.train",
            "--ue-executable",
            UE_CMD,
            "--project",
            UPROJECT,
            "--task",
            task_id,
            "--worker.slot_count",
            slot_count,
            "--worker.decimation",
            "[1,7]",
            "--runner.rollout_length",
            "4",
            "--runner.max_iterations",
            "1",
            "--logging.run_directory",
            str(run_directory),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=480,
        check=False,
    )
    output = f"{completed.stdout}\n{completed.stderr}"
    assert completed.returncode == 0, output[-6000:]
    assert (run_directory / "model_final.pt").is_file(), output[-2000:]


def test_phantomx_walk_export_produces_valid_uerlpol2(tmp_path: Path) -> None:
    """Train walk once, export UERLPOL2 from the DirectTask plan path, reload artifact."""

    run_directory = tmp_path / "walk-export-train"
    train = subprocess.run(
        [
            sys.executable,
            "-m",
            "uerl.cli.train",
            "--ue-executable",
            UE_CMD,
            "--project",
            UPROJECT,
            "--task",
            PHANTOMX_TASK_ID,
            "--worker.slot_count",
            "1",
            "--worker.decimation",
            "[1,7]",
            "--runner.rollout_length",
            "4",
            "--runner.max_iterations",
            "1",
            "--logging.run_directory",
            str(run_directory),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=480,
        check=False,
    )
    train_out = f"{train.stdout}\n{train.stderr}"
    assert train.returncode == 0, train_out[-6000:]
    checkpoint = run_directory / "model_final.pt"
    assert checkpoint.is_file()

    runtime_path = tmp_path / "robot_runtime.json"
    _write_robot_runtime(runtime_path)
    artifact_path = tmp_path / "phantomx_walk_composed.uerlpol2"
    export = subprocess.run(
        [
            sys.executable,
            "-m",
            "uerl.cli.export",
            "--ue-executable",
            UE_CMD,
            "--project",
            UPROJECT,
            "--task",
            PHANTOMX_TASK_ID,
            "--checkpoint",
            str(checkpoint),
            "--output",
            str(artifact_path),
            "--robot-runtime",
            str(runtime_path),
            "--presentation",
            "none",
            "--worker.slot_count",
            "1",
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=480,
        check=False,
    )
    export_out = f"{export.stdout}\n{export.stderr}"
    assert export.returncode == 0, export_out[-6000:]
    assert artifact_path.is_file()
    artifact = PolicyArtifact.read(artifact_path)
    artifact.validate()
    assert artifact.observation_plan.group_widths["policy"] == 109
    assert not any(
        "contact_force" in name for name in artifact.observation_plan.state_requirements
    )
    assert artifact.action_plan.policy_width == 18

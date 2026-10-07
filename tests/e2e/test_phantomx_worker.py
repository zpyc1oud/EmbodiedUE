"""Verify PhantomX through the generic Robot and shared-map runtime."""

from __future__ import annotations

import json
import subprocess
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import cast

import pytest
import torch

from tests.e2e.support.worker_runner import UE_CMD, UPROJECT
from uerl import PresentationMode, UERLDirectEnv, UERLSession, UERLSessionAdapter
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.phantomx import PHANTOMX_TERRAIN_TASK_ID
from uerl.tasks.registry import create_default_registry
from uerl.training import build_launch_overrides, build_run_config
from uerl.training.runner import ENVIRONMENT_DEVICE

REPO_ROOT = Path(__file__).resolve().parents[2]


def test_generic_phantomx_training_retains_position_targets(tmp_path: Path) -> None:
    """A short D4 rollout must use the generic runtime and apply each position target once."""

    run_directory = tmp_path / "generic-phantomx"
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "uerl.cli.train",
            "--task",
            "UERL-PhantomX-Walk-v0",
            "--ue-executable",
            UE_CMD,
            "--project",
            UPROJECT,
            "--worker.slot_count",
            "64",
            "--worker.decimation",
            "[4,4]",
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
        timeout=300,
        check=False,
    )

    output = f"{completed.stdout}\n{completed.stderr}"
    assert completed.returncode == 0, output[-4000:]
    assert (run_directory / "model_final.pt").is_file()
    checkpoint = torch.load(run_directory / "model_final.pt", map_location="cpu", weights_only=False)
    assert checkpoint["infos"]["uerl_training_objective"] == "phantomx_physical_time_v1"
    rows = [
        json.loads(line)
        for line in (run_directory / "worker_stage_latency.jsonl").read_text(encoding="utf-8").splitlines()
    ]
    step_rows = [row for row in rows if row["request_type"] == "step"]
    assert len(step_rows) == 4
    assert {int(row["action_apply_count"]) for row in step_rows} == {1}




def test_phantomx_contact_force_fields_round_trip_as_nonnegative_finite_values(tmp_path: Path) -> None:
    """The generic PhantomX schema returns all declared contact-force scalars."""
    run_directory = tmp_path / "phantomx-contact-force"
    config = build_run_config(
        PHANTOMX_TERRAIN_TASK_ID,
        overrides={
            **build_launch_overrides(
                ue_executable=Path(UE_CMD),
                project=Path(UPROJECT),
                map_name="/Engine/Maps/Entry",
                presentation=PresentationMode.NONE,
            ),
            "worker.slot_count": "64",
            "worker.run_seed": "0",
            "worker.decimation": "[4,4]",
            "task.max_episode_duration_s": "0.16",
            "logging.run_directory": str(run_directory),
        },
    )
    registry = create_default_registry()
    task = registry.create_task(config.task_id, config.task)
    curriculum_manager = registry.create_curriculum(
        config.task_id,
        config.task,
        num_envs=config.worker.slot_count,
        device=ENVIRONMENT_DEVICE,
        run_seed=config.worker.run_seed,
    )
    raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
    env: UERLDirectEnv | None = None
    try:
        adapter = UERLSessionAdapter(raw_session)
        env = UERLDirectEnv(
            adapter,
            task,
            resolved_config=config,
            curriculum_manager=curriculum_manager,
        )
        info: Mapping[str, object] = {}
        truncated = torch.zeros(64, dtype=torch.bool)
        for _ in range(8):
            _, _, _, truncated, info = env.step(torch.zeros(64, 18))
        assert torch.equal(truncated, torch.ones(64, dtype=torch.bool))
        terminal_raw_state = cast(Mapping[str, torch.Tensor], info["terminal_raw_state"])
        contact_force_names = [
            cast(str, item["name"])
            for item in task.schema.state_requirements
            if item["semantic"] == "contact_force"
        ]
        assert len(contact_force_names) == 7
        values = torch.cat([terminal_raw_state[name].reshape(64, -1) for name in contact_force_names], dim=1)
        assert torch.isfinite(values).all()
        assert (values >= 0.0).all()
        assert bool((values[:, 1:] > 0.0).any())
        print(
            f"[VERIFY] contact_force base_max={values[:, 0].max().item():.6f} "
            f"foot_max={values[:, 1:].max().item():.6f}"
        )
    finally:
        if env is not None:
            env.close("test_complete")
        else:
            raw_session.close("test_complete")


@pytest.mark.parametrize("terrain_level", [0, 4, 7])
def test_phantomx_default_pose_settles_without_spurious_reset(
    tmp_path: Path, terrain_level: int
) -> None:
    """The real 64-Slot default pose must settle without a startup grace period."""

    run_directory = tmp_path / f"phantomx-settle-level-{terrain_level}"
    config = build_run_config(
        PHANTOMX_TERRAIN_TASK_ID,
        overrides={
            **build_launch_overrides(
                ue_executable=Path(UE_CMD),
                project=Path(UPROJECT),
                map_name="/Engine/Maps/Entry",
                presentation=PresentationMode.NONE,
            ),
            "worker.slot_count": "64",
            "worker.run_seed": "0",
            "logging.run_directory": str(run_directory),
        },
    )
    registry = create_default_registry()
    task = registry.create_task(config.task_id, config.task)
    curriculum_manager = registry.create_curriculum(
        config.task_id,
        config.task,
        num_envs=config.worker.slot_count,
        device=ENVIRONMENT_DEVICE,
        run_seed=config.worker.run_seed,
    )
    raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
    env: UERLDirectEnv | None = None
    try:
        adapter = UERLSessionAdapter(raw_session)
        env = UERLDirectEnv(
            adapter,
            task,
            resolved_config=config,
            curriculum_manager=curriculum_manager,
            initial_terrain_level=terrain_level,
        )
        foot_support_observed = False
        for _ in range(25):
            _, _, terminated, truncated, info = env.step(torch.zeros(64, 18))
            done = terminated | truncated
            if bool(done.any()):
                done_ids = torch.where(done)[0]
                reasons = cast(Mapping[str, object], info["termination_reason"])
                diagnostics = {
                    name: value[done_ids].tolist()
                    for name, value in reasons.items()
                    if isinstance(value, torch.Tensor)
                }
                metrics = cast(Mapping[str, object], info["episode_metrics"])
                diagnostics.update(
                    {
                        name: cast(torch.Tensor, metrics[name])[done_ids].tolist()
                        for name in (
                            "phantomx/body_clearance",
                            "phantomx/body_height",
                            "phantomx/upright",
                            "phantomx/terrain_height_max",
                            "phantomx/terrain_height_min",
                            "phantomx/foot_contact_fraction",
                            "phantomx/reset_age_steps",
                        )
                    }
                )
                pytest.fail(f"level {terrain_level} terminated slots {done_ids.tolist()}: {diagnostics}")
            metrics = cast(Mapping[str, object], info["episode_metrics"])
            foot_fraction = cast(torch.Tensor, metrics["phantomx/foot_contact_fraction"])
            base_contact = cast(torch.Tensor, metrics["Episode_Termination/base_contact"])
            assert torch.equal(base_contact, torch.zeros(64))
            foot_support_observed |= bool((foot_fraction > 0.0).any())
        assert foot_support_observed
    finally:
        if env is not None:
            env.close("test_complete")
        else:
            raw_session.close("test_complete")

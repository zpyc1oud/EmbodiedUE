"""Verify PhantomX through the generic Robot and shared-map runtime."""

from __future__ import annotations

import json
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import cast

import pytest
import torch

from tests.e2e.support.worker_runner import UE_CMD, UPROJECT, run_owned_command
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
    completed = run_owned_command(
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
        timeout=300,
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




@pytest.mark.parametrize(
    ("task_id", "map_name"),
    [
        pytest.param(PHANTOMX_TERRAIN_TASK_ID, "/Engine/Maps/Entry", id="procedural"),
        pytest.param("UERL-PhantomX-Walk-v0", "/Game/Maps/NewMap", id="authored"),
    ],
)
def test_phantomx_contact_force_fields_round_trip_as_nonnegative_finite_values(
    tmp_path: Path, task_id: str, map_name: str,
) -> None:
    """Both procedural and authored floors produce a measured foot force."""
    run_directory = tmp_path / "phantomx-contact-force"
    config = build_run_config(
        task_id,
        overrides={
            **build_launch_overrides(
                ue_executable=Path(UE_CMD),
                project=Path(UPROJECT),
                map_name=map_name,
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
        assert bool((values[:, 1:] > 0.0).any()), (
            f"{task_id} on {map_name}: every measured foot force is zero after landing"
        )
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


def test_custom_reference_pose_matches_reset_observations_and_serialized_plan(tmp_path: Path) -> None:
    """A supported RobotAssetCfg reference pose must survive the real UE boundary."""
    from dataclasses import replace

    from uerl.assets.robots.phantomx import PHANTOMX_CFG, PHANTOMX_JOINTS
    from uerl.core.config.resolver import RunConfigResolver
    from uerl.core.mdp.executor import PlanExecutor, PlanInputs
    from uerl.core.mdp.plan import ObservationPlan
    from uerl.tasks.registry import TaskRegistry

    asset = replace(PHANTOMX_CFG, init_state=replace(
        PHANTOMX_CFG.init_state, joint_pos={"c1_.*": 0.03, "thigh_.*": 0.20, "tibia_.*": -0.35},
    ))
    base = create_default_registry().resolve("UERL-PhantomX-Walk-v0")
    registry = TaskRegistry()
    registry.register(replace(
        base, worker_config_factory=lambda: replace(
            base.worker_config_factory(), robot_semantics=asset.to_robot_config(),
        ),
    ))
    config = RunConfigResolver(registry).resolve(base.task_id, {
        # The shared-world Task requires its registered authored floor.
        **build_launch_overrides(
            ue_executable=Path(UE_CMD), project=Path(UPROJECT), map_name=base.session_config.map_path,
        ),
        "worker.slot_count": "1", "worker.decimation": "[4,4]",
        "logging.run_directory": str(tmp_path / "custom-reference"),
    })
    task = registry.create_task(config.task_id, config.task)
    raw = UERLSession.open(config, process_controller=WorkerProcessController())
    env = None
    try:
        env = UERLDirectEnv(UERLSessionAdapter(raw), task, resolved_config=config)
        plan = ObservationPlan.from_json(task.observation_plan.to_json())
        widths = {op.output: op.width for op in plan.ops}
        members = plan.groups["policy"]
        joint_start = sum(widths[name] for name in members[:members.index("joint_pos_rel")])
        defaults = torch.tensor([[0.03, 0.20, -0.35] * 6])
        reset_observations, _ = env.reset()
        assert torch.allclose(reset_observations["policy"][:, joint_start:joint_start + 18],
                              torch.zeros(1, 18), atol=2e-5, rtol=0.0)
        executor = PlanExecutor(plan, command_channels=task.command_source.channels())
        actions = torch.zeros(1, 18)
        for _ in range(5):
            _, _, _, _, info = env.step(actions)
            assert info["terminal_observation_valid"].tolist() == [True]
            assert info["slot_fault_code"].tolist() == [0]
            state = info["terminal_raw_state"]
            measured = torch.stack([state[f"robot.joint.{joint}.joint_position"].reshape(-1)
                                    for joint in PHANTOMX_JOINTS], dim=1)
            expected = measured - defaults
            terminal = info["terminal_observation"]["policy"]
            assert torch.allclose(terminal[:, joint_start:joint_start + 18], expected, atol=2e-5, rtol=0.0)
            exported = executor.execute(PlanInputs(
                raw_state=state, commands={"velocity": terminal[:, 9:12]}, previous_action=actions,
                control_frame_dt=torch.tensor([[info["transition_dt"]]]),
            ))
            assert torch.allclose(exported["policy"][:, joint_start:joint_start + 18], expected,
                                  atol=2e-5, rtol=0.0)
        reset_observations, _ = env.reset()
        assert torch.allclose(reset_observations["policy"][:, joint_start:joint_start + 18],
                              torch.zeros(1, 18), atol=2e-5, rtol=0.0)
    finally:
        if env is not None:
            env.close("custom_reference_complete")
        else:
            raw.close("custom_reference_setup_failed")

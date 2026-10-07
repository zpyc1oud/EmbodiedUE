"""E2E: startup EventManager term randomizes terrain levels through Session."""

from __future__ import annotations

from pathlib import Path
from typing import cast

import torch

from tests.e2e.support.worker_runner import UE_CMD, UPROJECT
from uerl import PresentationMode, UERLDirectEnv, UERLSession, UERLSessionAdapter
from uerl.core.config.robot import RobotSpec, RobotTopology
from uerl.core.mdp.lib.events import randomize_initial_terrain_levels
from uerl.core.mdp.managers.event import EventManager
from uerl.core.mdp.terms import EventCfg, EventTermCfg
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.phantomx import PHANTOMX_TERRAIN_TASK_ID
from uerl.tasks.registry import create_default_registry
from uerl.training import build_launch_overrides, build_run_config
from uerl.training.runner import ENVIRONMENT_DEVICE

SLOT_COUNT = 64


def _stub_robot_spec() -> RobotSpec:
    topology = RobotTopology(
        body_names=("base",),
        body_motion_types=("kinematic",),
        root_body_index=0,
        fixed_base=True,
        joints=(),
    )
    return RobotSpec(
        actuators=(),
        observations=(),
        reset=(),
        body_names=topology.body_names,
        topology=topology,
    )


def test_startup_terrain_level_event_reaches_session(tmp_path: Path) -> None:
    """AC: at least one protocol-backed startup event runs end-to-end."""

    run_directory = tmp_path / "event-startup-terrain"
    config = build_run_config(
        PHANTOMX_TERRAIN_TASK_ID,
        overrides={
            **build_launch_overrides(
                ue_executable=Path(UE_CMD),
                project=Path(UPROJECT),
                map_name="/Engine/Maps/Entry",
                presentation=PresentationMode.NONE,
            ),
            "worker.slot_count": str(SLOT_COUNT),
            "worker.run_seed": "7",
            "task.max_episode_steps": "2",
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
    generator = torch.Generator(device="cpu")
    generator.manual_seed(int(config.worker.run_seed))
    num_levels = int(cast(int, config.worker.terrain_config["num_levels"]))
    events = EventManager(
        EventCfg(
            terms={
                "startup_terrain": EventTermCfg(
                    func=randomize_initial_terrain_levels,
                    mode="startup",
                    params={"num_levels": num_levels},
                )
            }
        ),
        _stub_robot_spec(),
        batch_size=SLOT_COUNT,
        device=ENVIRONMENT_DEVICE,
        generator=generator,
    )
    raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
    env: UERLDirectEnv | None = None
    try:
        adapter = UERLSessionAdapter(raw_session, device=ENVIRONMENT_DEVICE)
        env = UERLDirectEnv(
            adapter,
            task,
            resolved_config=config,
            device=ENVIRONMENT_DEVICE,
            curriculum_manager=curriculum_manager,
            event_manager=events,
        )
        levels = env._terrain_levels.detach().cpu().to(dtype=torch.int64)
        assert levels.shape == (SLOT_COUNT,)
        assert bool((levels >= 0).all() and (levels < num_levels).all())
        # Seeded startup must not leave every Slot on level 0 for this range.
        unique_count = int(torch.unique(levels).numel())
        assert unique_count >= 2
        _, _, _, _, info = env.step(torch.zeros(SLOT_COUNT, task.num_actions))
        assert "terrain_level" in info
        print(
            f"[VERIFY] EVENT-STARTUP: unique_levels={unique_count} "
            f"min={int(levels.min())} max={int(levels.max())}"
        )
    finally:
        if env is not None:
            env.close("event_startup_complete")
        else:
            raw_session.close("event_startup_complete")

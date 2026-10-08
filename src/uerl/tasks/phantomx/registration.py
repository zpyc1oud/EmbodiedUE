"""Register the PhantomX walking task in the explicit Python registry."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import fields, replace
from math import ceil, hypot
from pathlib import Path
from typing import cast

import torch

from ...core.config import LoggingConfig, RslRlRunnerConfig, SessionConfig, WorkerConfig
from ...core.config.models import DirectTaskConfig
from ...core.config.robot import RobotSpec
from ...core.direct.curriculum import CurriculumManager
from ...core.direct.robot_observation import ObservationShapeTable
from ...core.mdp.lib.events import push_root, randomize_ground_friction
from ...core.mdp.managers.event import EventManager
from ...core.mdp.terms import EventCfg, EventTermCfg
from ..registry.models import TaskRegistration
from ..registry.tasks import TaskRegistry
from .catch import PhantomXCatchTaskConfig, create_phantomx_catch_direct_task
from .catch_evaluation import CatchEvaluator, format_catch_evaluation
from .config import (
    PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH,
    PHANTOMX_DISCRETE_TERRAIN_ENVIRONMENT_ID,
    PHANTOMX_DISCRETE_TERRAIN_TASK_ID,
    PHANTOMX_ENVIRONMENT_ID,
    PHANTOMX_PURSUIT_ENVIRONMENT_ID,
    PHANTOMX_PURSUIT_TASK_ID,
    PHANTOMX_ROBOT_ID,
    PHANTOMX_SLOT_ISOLATED_ENVIRONMENT_ID,
    PHANTOMX_TASK_ID,
    PHANTOMX_TASK_VERSION,
    PHANTOMX_TERRAIN_CONFIG_PATH,
    PHANTOMX_TERRAIN_ENVIRONMENT_ID,
    PHANTOMX_TERRAIN_TASK_ID,
    PhantomXTaskConfig,
    load_phantomx_discrete_terrain_config,
    load_phantomx_terrain_config,
    load_phantomx_training_config,
)
from .curriculum import create_phantomx_curriculum
from .evaluation import PhantomXEvaluator, format_phantomx_evaluation
from .task import (
    PhantomXTask,
    create_phantomx_continuous_terrain_direct_task,
    create_phantomx_direct_task,
    create_phantomx_discrete_terrain_direct_task,
    create_phantomx_pursuit_direct_task,
)

PHANTOMX_SHARED_SPAWN_SPAN_M = 9.0
PHANTOMX_TERRAIN_SCAN_REACH_M = hypot(2.0, 0.6)


def create_phantomx_task_config() -> PhantomXTaskConfig:
    return load_phantomx_training_config().task


def create_phantomx_worker_config() -> WorkerConfig:
    return load_phantomx_training_config().worker


def create_phantomx_pursuit_worker_config() -> WorkerConfig:
    """Claim the authored PhantomX and place its one Slot on the Stylized Egypt floor."""

    return replace(
        create_phantomx_worker_config(),
        slot_count=1,
        environment_id=PHANTOMX_PURSUIT_ENVIRONMENT_ID,
        environment_config={
            "environment.origin_x_m": -9.4,
            "environment.origin_y_m": 1.6,
            "environment.trace_start_z_m": 10.0,
            "environment.trace_depth_m": 20.0,
        },
        robot_config={"robot.claim_authored_actor": 1.0},
        terrain_config={},
        terrain_config_path=None,
    )


def create_phantomx_terrain_worker_config(*, isolated: bool = True) -> WorkerConfig:
    """Build the continuous route with enough patch for a full episode."""

    shared_spawn_span = None if isolated else PHANTOMX_SHARED_SPAWN_SPAN_M
    terrain_config = _expand_phantomx_terrain_patch(
        load_phantomx_terrain_config(), shared_spawn_span=shared_spawn_span
    )
    return _create_phantomx_terrain_worker_config(
        terrain_config,
        terrain_config_path=PHANTOMX_TERRAIN_CONFIG_PATH,
        isolated=isolated,
        shared_environment_id=PHANTOMX_ENVIRONMENT_ID,
        shared_spawn_span=shared_spawn_span,
    )


def create_phantomx_discrete_terrain_worker_config(*, isolated: bool = False) -> WorkerConfig:
    """Train 64 Robots on a shared discrete terrain atlas by default."""

    shared_spawn_span = None if isolated else PHANTOMX_SHARED_SPAWN_SPAN_M
    terrain_config = load_phantomx_discrete_terrain_config()
    if not isolated:
        terrain_config = _expand_phantomx_terrain_patch(
            terrain_config, shared_spawn_span=shared_spawn_span
        )
    return _create_phantomx_terrain_worker_config(
        terrain_config,
        terrain_config_path=PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH,
        isolated=isolated,
        shared_environment_id=PHANTOMX_DISCRETE_TERRAIN_ENVIRONMENT_ID,
        shared_spawn_span=shared_spawn_span,
    )


def _create_phantomx_terrain_worker_config(
    terrain_config: Mapping[str, object],
    terrain_config_path: str,
    *,
    isolated: bool,
    shared_environment_id: str,
    shared_spawn_span: float | None = None,
) -> WorkerConfig:
    """Build the selected atlas route for one terrain source."""

    base_worker_config = create_phantomx_worker_config()
    cell_size = cast(Sequence[float], terrain_config["cell_size"])
    border_width = float(cast(float, terrain_config["border_width"]))
    num_levels = int(cast(int, terrain_config["num_levels"]))
    terrain_span_x = float(cell_size[0]) + 2.0 * border_width
    terrain_span_y = float(cell_size[1]) + 2.0 * border_width
    slot_count = 64
    columns = 8
    rows = ceil(slot_count / columns)
    if isolated:
        # Slot-isolated generation keeps every pre-generated level of one Slot
        # together, but is intentionally an explicit small-patch option.
        slot_gap = 0.4
        spacing_x = num_levels * terrain_span_x + slot_gap
        spacing_y = terrain_span_y + slot_gap
        origin_x = -0.5 * ((columns - 1) * spacing_x + (num_levels - 1) * terrain_span_x)
        origin_y = -0.5 * ((rows - 1) * spacing_y)
        environment_id = PHANTOMX_SLOT_ISOLATED_ENVIRONMENT_ID
    else:
        # SharedWorld keeps the spawn lattice in the centre of the enlarged
        # atlas. Its size is chosen independently, so enlarging the atlas does
        # not move the 64 starts back out toward its edge.
        if shared_spawn_span is None:
            slot_margin = 0.5
            usable_x = float(cell_size[0]) - 2.0 * border_width - 2.0 * slot_margin
            usable_y = float(cell_size[1]) - 2.0 * border_width - 2.0 * slot_margin
        else:
            usable_x = shared_spawn_span
            usable_y = shared_spawn_span
        spacing_x = usable_x / float(columns - 1)
        spacing_y = usable_y / float(rows - 1)
        origin_x = -0.5 * usable_x
        origin_y = -0.5 * usable_y
        environment_id = shared_environment_id

    environment_config: dict[str, object] = {
        "environment.spacing_x_m": spacing_x,
        "environment.spacing_y_m": spacing_y,
        "environment.columns": float(columns),
        "environment.origin_x_m": origin_x,
        "environment.origin_y_m": origin_y,
    }
    if not isolated:
        environment_config.update(
            {
                "environment.trace_start_z_m": cast(
                    float, base_worker_config.environment_config["environment.trace_start_z_m"]
                ),
                "environment.trace_depth_m": cast(
                    float, base_worker_config.environment_config["environment.trace_depth_m"]
                ),
            }
        )

    return replace(
        base_worker_config,
        slot_count=slot_count,
        environment_id=environment_id,
        environment_config=environment_config,
        terrain_config=terrain_config,
        terrain_config_path=Path(terrain_config_path),
    )


def _expand_phantomx_terrain_patch(
    terrain_config: Mapping[str, object],
    *,
    shared_spawn_span: float | None,
) -> Mapping[str, object]:
    """Size a generated terrain patch for one complete episode.

    The root may travel at the configured maximum command speed for the full
    episode. The scan reach covers the farthest of the existing nine probes,
    and one raster interval keeps the probe away from the mesh edge. Isolated
    terrain resets at the patch centre; SharedWorld terrain spans a fixed
    spawn lattice and keeps that lattice centred as the patch grows.
    """

    training = load_phantomx_training_config()
    max_speed = max(
        training.task.command.initial_speed_max,
        training.task.command.post_turn_speed_max,
    )
    duration = training.task.max_episode_duration_s
    assert duration is not None
    max_travel = max_speed * duration
    tiers = cast(Sequence[Mapping[str, object]], terrain_config["tiers"])
    primitive = cast(str, tiers[0]["primitive"])
    raster_key = "grid_width" if primitive == "boxes" else "horizontal_scale"
    raster_width = max(
        float(cast(float, cast(Mapping[str, object], tier["params"])[raster_key]))
        for tier in tiers
    )
    required_half_span = max_travel + PHANTOMX_TERRAIN_SCAN_REACH_M + raster_width
    required_patch_span = (
        2.0 * required_half_span
        if shared_spawn_span is None
        else shared_spawn_span + 2.0 * required_half_span
    )
    border_width = float(cast(float, terrain_config["border_width"]))
    base_cell_size = cast(Sequence[float], terrain_config["cell_size"])
    cell_size = [max(float(base_cell_size[Axis]), ceil(required_patch_span - 2.0 * border_width)) for Axis in range(2)]
    return {**terrain_config, "cell_size": cell_size}


def create_phantomx_runner_config() -> RslRlRunnerConfig:
    return load_phantomx_training_config().runner


def create_phantomx_terrain_runner_config() -> RslRlRunnerConfig:
    runner = create_phantomx_runner_config()
    return replace(
        runner,
        parameters={**runner.parameters, "run_name": "phantomx_continuous_terrain"},
    )


def create_phantomx_discrete_terrain_runner_config() -> RslRlRunnerConfig:
    runner = create_phantomx_runner_config()
    return replace(
        runner,
        parameters={**runner.parameters, "run_name": "phantomx_discrete_terrain"},
    )


def create_phantomx_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    training = load_phantomx_training_config()
    control_dt = float(training.worker.physics_dt) * training.worker.decimation[1]
    return create_phantomx_direct_task(
        config,
        robot_spec=robot_spec,
        control_dt=control_dt,
        batch_size=int(training.worker.slot_count),
        device="cpu",
        observation_shapes=observation_shapes,
    )


def create_phantomx_continuous_terrain_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    training = load_phantomx_training_config()
    control_dt = float(training.worker.physics_dt) * training.worker.decimation[1]
    return create_phantomx_continuous_terrain_direct_task(
        config,
        robot_spec=robot_spec,
        control_dt=control_dt,
        batch_size=int(training.worker.slot_count),
        device="cpu",
        observation_shapes=observation_shapes,
    )


def create_phantomx_discrete_terrain_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    training = load_phantomx_training_config()
    control_dt = float(training.worker.physics_dt) * training.worker.decimation[1]
    return create_phantomx_discrete_terrain_direct_task(
        config,
        robot_spec=robot_spec,
        control_dt=control_dt,
        batch_size=int(training.worker.slot_count),
        device="cpu",
        observation_shapes=observation_shapes,
    )


def create_phantomx_pursuit_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    training = load_phantomx_training_config()
    control_dt = float(training.worker.physics_dt) * training.worker.decimation[1]
    return create_phantomx_pursuit_direct_task(
        config,
        robot_spec=robot_spec,
        control_dt=control_dt,
        batch_size=1,
        device="cpu",
        observation_shapes=observation_shapes,
    )


def create_phantomx_curriculum_manager(
    config: DirectTaskConfig, num_envs: int, device: str, run_seed: int
) -> CurriculumManager:
    task_config = PhantomXTaskConfig.from_direct(config)
    return create_phantomx_curriculum(
        task_config.curriculum,
        task_config.command,
        num_envs=num_envs,
        device=device,
        run_seed=run_seed,
    )


def create_phantomx_event_manager(
    config: DirectTaskConfig,
    spec: RobotSpec,
    *,
    batch_size: int,
    device: str | torch.device,
    run_seed: int,
) -> EventManager:
    """Friction at startup and a root velocity push on an interval. No terrain roughness."""

    task = PhantomXTaskConfig.from_direct(config)
    events = task.events
    generator = torch.Generator(device="cpu")
    generator.manual_seed(run_seed)
    return EventManager(
        EventCfg(
            terms={
                "ground_friction": EventTermCfg(
                    func=randomize_ground_friction,
                    mode="startup",
                    params={
                        "static_range": (events.static_friction_min, events.static_friction_max),
                        "dynamic_range": (events.dynamic_friction_min, events.dynamic_friction_max),
                    },
                ),
                "root_push": EventTermCfg(
                    func=push_root,
                    mode="interval",
                    interval_range_s=(events.push_interval_min_s, events.push_interval_max_s),
                    params={
                        "velocity_range_mps": (
                            events.push_velocity_min_mps,
                            events.push_velocity_max_mps,
                        ),
                    },
                ),
            }
        ),
        spec,
        batch_size=batch_size,
        device=device,
        generator=generator,
    )


def create_phantomx_registration() -> TaskRegistration:
    return TaskRegistration(
        task_id=PHANTOMX_TASK_ID,
        task_version=PHANTOMX_TASK_VERSION,
        environment_id=PHANTOMX_ENVIRONMENT_ID,
        robot_id=PHANTOMX_ROBOT_ID,
        task_factory=create_phantomx_task,
        worker_config_factory=create_phantomx_worker_config,
        task_config_factory=create_phantomx_task_config,
        runner_config_factory=create_phantomx_runner_config,
        session_config=SessionConfig(map_path="/Game/Maps/NewMap"),
        logging_config=LoggingConfig(),
        curriculum_factory=create_phantomx_curriculum_manager,
        evaluation_factory=PhantomXEvaluator,
        evaluation_formatter=format_phantomx_evaluation,
        event_manager_factory=create_phantomx_event_manager,
    )


def create_phantomx_pursuit_registration() -> TaskRegistration:
    return TaskRegistration(
        task_id=PHANTOMX_PURSUIT_TASK_ID,
        task_version=PHANTOMX_TASK_VERSION,
        environment_id=PHANTOMX_PURSUIT_ENVIRONMENT_ID,
        robot_id=PHANTOMX_ROBOT_ID,
        task_factory=create_phantomx_pursuit_task,
        worker_config_factory=create_phantomx_pursuit_worker_config,
        task_config_factory=create_phantomx_task_config,
        runner_config_factory=create_phantomx_discrete_terrain_runner_config,
        session_config=SessionConfig(map_path="/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo"),
        logging_config=LoggingConfig(),
        curriculum_factory=create_phantomx_curriculum_manager,
        evaluation_factory=PhantomXEvaluator,
        evaluation_formatter=format_phantomx_evaluation,
        event_manager_factory=create_phantomx_event_manager,
    )


def create_phantomx_terrain_registration() -> TaskRegistration:
    return TaskRegistration(
        task_id=PHANTOMX_TERRAIN_TASK_ID,
        task_version=PHANTOMX_TASK_VERSION,
        environment_id=PHANTOMX_TERRAIN_ENVIRONMENT_ID,
        robot_id=PHANTOMX_ROBOT_ID,
        task_factory=create_phantomx_continuous_terrain_task,
        worker_config_factory=create_phantomx_terrain_worker_config,
        task_config_factory=create_phantomx_task_config,
        runner_config_factory=create_phantomx_terrain_runner_config,
        session_config=SessionConfig(map_path="/Engine/Maps/Entry"),
        logging_config=LoggingConfig(),
        curriculum_factory=create_phantomx_curriculum_manager,
        evaluation_factory=PhantomXEvaluator,
        evaluation_formatter=format_phantomx_evaluation,
        event_manager_factory=create_phantomx_event_manager,
    )


def create_phantomx_discrete_terrain_registration() -> TaskRegistration:
    return TaskRegistration(
        task_id=PHANTOMX_DISCRETE_TERRAIN_TASK_ID,
        task_version=PHANTOMX_TASK_VERSION,
        environment_id=PHANTOMX_DISCRETE_TERRAIN_ENVIRONMENT_ID,
        robot_id=PHANTOMX_ROBOT_ID,
        task_factory=create_phantomx_discrete_terrain_task,
        worker_config_factory=create_phantomx_discrete_terrain_worker_config,
        task_config_factory=create_phantomx_task_config,
        runner_config_factory=create_phantomx_discrete_terrain_runner_config,
        session_config=SessionConfig(map_path="/Engine/Maps/Entry"),
        logging_config=LoggingConfig(),
        curriculum_factory=create_phantomx_curriculum_manager,
        evaluation_factory=PhantomXEvaluator,
        evaluation_formatter=format_phantomx_evaluation,
        event_manager_factory=create_phantomx_event_manager,
    )


def create_phantomx_catch_registration() -> TaskRegistration:
    """Register the single-Robot procedural Catch arena and contact objective."""

    def config_factory() -> PhantomXCatchTaskConfig:
        base = create_phantomx_task_config()
        return PhantomXCatchTaskConfig(**{item.name: getattr(base, item.name) for item in fields(base)})

    def worker_factory() -> WorkerConfig:
        return replace(
            create_phantomx_worker_config(), slot_count=1,
            environment_id="uerl.environment.catch",
            environment_config={
                "environment.target_x_m": 1.5,
                "environment.target_y_m": 0.0,
                "environment.target_speed_mps": 0.15,
                "environment.target_path_radius_m": 0.5,
                "environment.use_player_target": 0.0,
            },
        )

    def task_factory(
        config: DirectTaskConfig, *, robot_spec: RobotSpec | None = None,
        observation_shapes: ObservationShapeTable | None = None,
    ) -> PhantomXTask:
        training = load_phantomx_training_config()
        return create_phantomx_catch_direct_task(
            config, robot_spec=robot_spec, observation_shapes=observation_shapes,
            control_dt=training.worker.physics_dt * training.worker.decimation[0],
            batch_size=1, device="cpu",
        )

    return TaskRegistration(
        task_id="UERL-PhantomX-Catch-v0", task_version=PHANTOMX_TASK_VERSION,
        environment_id="uerl.environment.catch", robot_id=PHANTOMX_ROBOT_ID,
        task_factory=task_factory, worker_config_factory=worker_factory,
        task_config_factory=config_factory, runner_config_factory=create_phantomx_runner_config,
        session_config=SessionConfig(map_path="/Engine/Maps/Entry"), logging_config=LoggingConfig(),
        evaluation_factory=CatchEvaluator, evaluation_formatter=format_catch_evaluation,
        event_manager_factory=create_phantomx_event_manager,
    )


def register_phantomx(registry: TaskRegistry) -> None:
    registry.register(create_phantomx_registration())
    registry.register(create_phantomx_catch_registration())
    registry.register(create_phantomx_pursuit_registration())
    registry.register(create_phantomx_terrain_registration())
    registry.register(create_phantomx_discrete_terrain_registration())


__all__ = [
    "create_phantomx_registration",
    "create_phantomx_pursuit_registration",
    "create_phantomx_pursuit_task",
    "create_phantomx_pursuit_worker_config",
    "create_phantomx_terrain_registration",
    "create_phantomx_discrete_terrain_registration",
    "create_phantomx_discrete_terrain_runner_config",
    "create_phantomx_discrete_terrain_worker_config",
    "create_phantomx_terrain_runner_config",
    "create_phantomx_terrain_worker_config",
    "create_phantomx_runner_config",
    "create_phantomx_curriculum_manager",
    "create_phantomx_task",
    "create_phantomx_continuous_terrain_task",
    "create_phantomx_discrete_terrain_task",
    "create_phantomx_task_config",
    "create_phantomx_worker_config",
    "register_phantomx",
]

"""Define the typed PhantomX walking task configuration and configspec loading."""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

import yaml

from ...assets.robots import get_robot_asset
from ...assets.robots.phantomx import PHANTOMX_ASSET_PATH, PHANTOMX_FEET, PHANTOMX_JOINTS
from ...core.config.configspec import MISSING, configspec, from_mapping, spec_field, to_dict
from ...core.config.models import DirectTaskConfig, RslRlRunnerConfig, WorkerConfig, validate_decimation_int32_range
from ...core.config.paths import repository_config_root
from ...core.config.robot import RobotConfig, RobotSpec, RobotTopology, merge_robot_spec
from ...core.config.terrain_spec import parse_terrain_config
from ...core.config.training_spec import RobotRefCfg, RslRlObsGroupsCfg, RslRlRunnerCfg
from ...core.config.yaml_loader import load_unique_yaml
from ...errors import ConfigError

PHANTOMX_TASK_ID = "UERL-PhantomX-Walk-v0"
PHANTOMX_TERRAIN_TASK_ID = "UERL-PhantomX-ContinuousTerrain-v0"
PHANTOMX_DISCRETE_TERRAIN_TASK_ID = "UERL-PhantomX-DiscreteTerrain-v0"
PHANTOMX_PURSUIT_TASK_ID = "UERL-PhantomX-Pursuit-v0"
PHANTOMX_TASK_VERSION = "2.0.0"
PHANTOMX_ENVIRONMENT_ID = "uerl.environment.shared_world"
PHANTOMX_SLOT_ISOLATED_ENVIRONMENT_ID = "uerl.environment.slot_isolated"
PHANTOMX_TERRAIN_ENVIRONMENT_ID = PHANTOMX_SLOT_ISOLATED_ENVIRONMENT_ID
PHANTOMX_DISCRETE_TERRAIN_ENVIRONMENT_ID = PHANTOMX_ENVIRONMENT_ID
PHANTOMX_PURSUIT_ENVIRONMENT_ID = "uerl.environment.authored_pursuit"
PHANTOMX_ROBOT_ID = "uerl.robot.skeletal_mesh"
PHANTOMX_TERRAIN_CONFIG_PATH = "environments/terrains/phantomx/continuous.yaml"
PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH = "environments/terrains/phantomx/discrete.yaml"
# The policy receives the completed control interval in milliseconds.  Keeping
# the scale beside the task contract makes the training declaration and the
# exported deployment plan use the same unit conversion.
PHANTOMX_CONTROL_FRAME_DT_SCALE = 100.0

PHANTOMX_COMMAND_FIELDS = (
    "task.command.initial_linear_velocity",
    "task.command.heading",
    "task.command.post_turn_linear_velocity",
)

_ENV_SCALAR_YAML_TO_FIELD = {
    "environment.spacing_x_m": "spacing_x_m",
    "environment.spacing_y_m": "spacing_y_m",
    "environment.columns": "columns",
    "environment.origin_x_m": "origin_x_m",
    "environment.origin_y_m": "origin_y_m",
    "environment.trace_start_z_m": "trace_start_z_m",
    "environment.trace_depth_m": "trace_depth_m",
}
_ENV_SCALAR_FIELD_TO_YAML = {value: key for key, value in _ENV_SCALAR_YAML_TO_FIELD.items()}


@dataclass(frozen=True, slots=True)
class PhantomXCurriculumConfig:
    """Configure automatic promotion from straight walking to turning."""

    window_episodes: int = 512
    minimum_episodes: int = 128
    promotion_success_rate: float = 0.6
    velocity_error_threshold: float = 0.20


@dataclass(frozen=True, slots=True)
class PhantomXCommandConfig:
    """Configure episode-boundary command sampling owned by Python."""

    initial_speed_min: float = 0.4
    initial_speed_max: float = 0.5
    post_turn_speed_min: float = 0.4
    post_turn_speed_max: float = 0.5
    heading_delta_min: float = -math.pi
    heading_delta_max: float = math.pi
    resampling_time_min_s: float = 5.0
    resampling_time_max_s: float = 10.0
    standing_probability: float = 0.1


@dataclass(frozen=True, slots=True)
class PhantomXObservationNoiseConfig:
    """Training-only Gaussian noise on actor proprioception. Not part of the plan."""

    lin_vel_b: float = 0.05
    ang_vel_b: float = 0.1
    gravity_b: float = 0.025
    joint_pos_rel: float = 0.01
    joint_vel: float = 0.05


@dataclass(frozen=True, slots=True)
class PhantomXTrainingEventConfig:
    """Domain randomization applied by training, not by play or evaluation."""

    static_friction_min: float = 0.7
    static_friction_max: float = 1.1
    dynamic_friction_min: float = 0.5
    dynamic_friction_max: float = 0.9
    push_velocity_min_mps: float = -0.2
    push_velocity_max_mps: float = 0.2
    push_interval_min_s: float = 1.0
    push_interval_max_s: float = 1.5


@dataclass(frozen=True, slots=True)
class PhantomXTaskConfig(DirectTaskConfig):
    """Hold walking reward, termination and action settings."""

    state_requirements: tuple[str, ...] = ()
    action_schema: tuple[str, ...] = ()
    max_episode_steps: int = 0
    max_episode_duration_s: float | None = 20.0
    reference_dt_s: float | None = 0.02
    slot_fault_reward: float = -10.0
    action_clip: float = 1.0
    velocity_tracking_std: float = 0.20
    yaw_rate_tracking_std: float = 0.5
    linear_velocity_tracking_weight: float = 1.5
    linear_velocity_progress_weight: float = 1.0
    yaw_rate_tracking_weight: float = 0.75
    yaw_rate_progress_weight: float = 0.75
    heading_control_stiffness: float = 0.5
    max_yaw_rate: float = 1.0
    turn_start_distance: float = 0.5
    turn_completion_tolerance: float = 0.1
    command: PhantomXCommandConfig = field(default_factory=PhantomXCommandConfig)
    curriculum: PhantomXCurriculumConfig = field(default_factory=PhantomXCurriculumConfig)
    moving_command_threshold: float = 0.1
    vertical_velocity_weight: float = 2.0
    body_angular_velocity_weight: float = 0.05
    upright_weight: float = 2.5
    body_clearance_weight: float = 200.0
    # Per 0.02 s reference frame on ||a_k - a_(k-1)||²; derivation beside
    # ``task.reward.action_rate`` in configs/tasks/phantomx/training.yaml.
    action_rate_weight: float = 2.4e-3
    joint_velocity_penalty: float = 0.005
    fall_penalty: float = 1.0
    body_clearance_penalty_threshold: float = 0.12
    base_contact_force_threshold: float = 1.0
    observation_noise: PhantomXObservationNoiseConfig = field(
        default_factory=PhantomXObservationNoiseConfig
    )
    events: PhantomXTrainingEventConfig = field(default_factory=PhantomXTrainingEventConfig)

    @classmethod
    def from_direct(cls, config: DirectTaskConfig) -> PhantomXTaskConfig:
        if isinstance(config, cls):
            return config
        return cls(
            parameters=config.parameters,
            max_episode_steps=config.max_episode_steps,
            max_episode_duration_s=(
                config.max_episode_duration_s
                if config.max_episode_duration_s is not None
                else config.max_episode_steps * 0.02 if config.max_episode_steps > 0 else None
            ),
            reference_dt_s=config.reference_dt_s or 0.02,
            slot_fault_reward=config.slot_fault_reward,
        )


@dataclass(frozen=True, slots=True)
class PhantomXTrainingConfig:
    """Hold the asset-library PhantomX Robot and YAML task partitions."""

    robot_asset_path: str
    robot_config_path: Path
    robot_config: RobotConfig
    task: PhantomXTaskConfig
    worker: WorkerConfig
    runner: RslRlRunnerConfig

    def resolve_robot_spec(self, topology: Mapping[str, object]) -> RobotSpec:
        return merge_robot_spec(self.robot_config, RobotTopology.from_response(topology))


DEFAULT_PHANTOMX_TRAINING_CONFIG = repository_config_root() / "tasks" / "phantomx" / "training.yaml"
DEFAULT_PHANTOMX_TERRAIN_CONFIG = (
    repository_config_root() / "environments" / "terrains" / "phantomx" / "continuous.yaml"
)
DEFAULT_PHANTOMX_DISCRETE_TERRAIN_CONFIG = (
    repository_config_root() / "environments" / "terrains" / "phantomx" / "discrete.yaml"
)


@configspec
class PhantomXIdentityCfg:
    """YAML identity partition with PhantomX registration constants."""

    task_id: Literal["UERL-PhantomX-Walk-v0"] = MISSING
    task_version: Literal["2.0.0"] = MISSING
    environment_id: Literal["uerl.environment.shared_world"] = MISSING
    robot_id: Literal["uerl.robot.skeletal_mesh"] = MISSING


@configspec
class PhantomXCommandCfg:
    """Episode-boundary command sampling ranges."""

    initial_speed_min: float = spec_field(MISSING, ge=0.0, finite=True)
    initial_speed_max: float = spec_field(MISSING, finite=True)
    post_turn_speed_min: float = spec_field(MISSING, ge=0.0, finite=True)
    post_turn_speed_max: float = spec_field(MISSING, finite=True)
    heading_delta_min: float = spec_field(MISSING, finite=True)
    heading_delta_max: float = spec_field(MISSING, finite=True)
    resampling_time_min_s: float = spec_field(MISSING, gt=0.0, finite=True)
    resampling_time_max_s: float = spec_field(MISSING, gt=0.0, finite=True)
    standing_probability: float = spec_field(MISSING, ge=0.0, le=1.0, finite=True)


@configspec
class PhantomXCurriculumCfg:
    """Straight-to-turn promotion thresholds."""

    window_episodes: int = spec_field(MISSING, gt=0, integral=True)
    minimum_episodes: int = spec_field(MISSING, gt=0, integral=True)
    promotion_success_rate: float = spec_field(MISSING, ge=0.0, le=1.0, finite=True)
    velocity_error_threshold: float = spec_field(MISSING, gt=0.0, finite=True)


@configspec
class PhantomXObservationNoiseCfg:
    """YAML partition for training-only actor noise."""

    lin_vel_b: float = spec_field(MISSING, ge=0.0, finite=True)
    ang_vel_b: float = spec_field(MISSING, ge=0.0, finite=True)
    gravity_b: float = spec_field(MISSING, ge=0.0, finite=True)
    joint_pos_rel: float = spec_field(MISSING, ge=0.0, finite=True)
    joint_vel: float = spec_field(MISSING, ge=0.0, finite=True)


@configspec
class PhantomXTrainingEventCfg:
    """YAML partition for training-only friction and root push."""

    static_friction_min: float = spec_field(MISSING, ge=0.0, finite=True)
    static_friction_max: float = spec_field(MISSING, ge=0.0, finite=True)
    dynamic_friction_min: float = spec_field(MISSING, ge=0.0, finite=True)
    dynamic_friction_max: float = spec_field(MISSING, ge=0.0, finite=True)
    push_velocity_min_mps: float = spec_field(MISSING, finite=True)
    push_velocity_max_mps: float = spec_field(MISSING, finite=True)
    push_interval_min_s: float = spec_field(MISSING, gt=0.0, finite=True)
    push_interval_max_s: float = spec_field(MISSING, gt=0.0, finite=True)


@configspec
class PhantomXRewardCfg:
    """Reward weight partition under ``task.reward``."""

    linear_velocity_tracking: float = spec_field(MISSING, finite=True)
    linear_velocity_progress: float = spec_field(MISSING, finite=True)
    yaw_rate_tracking: float = spec_field(MISSING, finite=True)
    yaw_rate_progress: float = spec_field(MISSING, finite=True)
    vertical_velocity: float = spec_field(MISSING, finite=True)
    body_angular_velocity: float = spec_field(MISSING, finite=True)
    upright: float = spec_field(MISSING, finite=True)
    body_clearance: float = spec_field(MISSING, gt=0.0, finite=True)
    action_rate: float = spec_field(MISSING, gt=0.0, finite=True)
    joint_velocity: float = spec_field(MISSING, finite=True)
    fall: float = spec_field(MISSING, gt=0.0, finite=True)


@configspec
class PhantomXTaskCfg:
    """YAML task partition for Direct PhantomX walking."""

    max_episode_duration_s: float = spec_field(MISSING, gt=0.0, finite=True)
    reference_dt_s: float = spec_field(MISSING, gt=0.0, finite=True)
    slot_fault_reward: float = spec_field(MISSING, finite=True)
    action_clip: float = spec_field(MISSING, gt=0.0, finite=True)
    velocity_tracking_std: float = spec_field(MISSING, gt=0.0, finite=True)
    yaw_rate_tracking_std: float = spec_field(MISSING, gt=0.0, finite=True)
    heading_control_stiffness: float = spec_field(MISSING, gt=0.0, finite=True)
    max_yaw_rate: float = spec_field(MISSING, gt=0.0, finite=True)
    turn_start_distance: float = spec_field(MISSING, gt=0.0, finite=True)
    turn_completion_tolerance: float = spec_field(MISSING, gt=0.0, finite=True)
    command: PhantomXCommandCfg = MISSING
    curriculum: PhantomXCurriculumCfg = MISSING
    observation_noise: PhantomXObservationNoiseCfg = MISSING
    events: PhantomXTrainingEventCfg = MISSING
    moving_command_threshold: float = spec_field(MISSING, gt=0.0, finite=True)  # type: ignore[misc]
    body_clearance_penalty_threshold: float = spec_field(MISSING, gt=0.0, finite=True)  # type: ignore[misc]
    base_contact_force_threshold: float = spec_field(MISSING, gt=0.0, finite=True)  # type: ignore[misc]
    reward: PhantomXRewardCfg = MISSING


@configspec
class PhantomXEnvironmentScalarsCfg:
    """Shared-world placement scalars after dotted YAML keys are rewritten."""

    spacing_x_m: float = spec_field(MISSING, gt=0.0, finite=True)
    spacing_y_m: float = spec_field(MISSING, gt=0.0, finite=True)
    columns: float = spec_field(MISSING, ge=1.0, finite=True)
    origin_x_m: float = spec_field(MISSING, finite=True)
    origin_y_m: float = spec_field(MISSING, finite=True)
    trace_start_z_m: float = spec_field(MISSING, finite=True)
    trace_depth_m: float = spec_field(MISSING, gt=0.0, finite=True)


@configspec
class PhantomXEnvironmentCfg:
    """Worker environment identity and scalars."""

    id: Literal["uerl.environment.shared_world"] = MISSING
    scalars: PhantomXEnvironmentScalarsCfg = MISSING


@configspec
class PhantomXWorkerCfg:
    """YAML worker partition projected into ``WorkerConfig``."""

    slot_count: int = spec_field(MISSING, gt=0, le=65536, integral=True)
    physics_dt: float = spec_field(MISSING, gt=0.0, le=1.0, finite=True)
    decimation: tuple[int, int] = spec_field(MISSING, item_gt=0, ordered=True)
    environment: PhantomXEnvironmentCfg = MISSING
    run_seed: int = spec_field(MISSING, ge=0, le=2**64 - 1, integral=True)  # type: ignore[misc]


@configspec
class PhantomXTrainingCfg:
    """Top-level PhantomX training YAML document."""

    identity: PhantomXIdentityCfg = MISSING
    robot: RobotRefCfg = MISSING
    task: PhantomXTaskCfg = MISSING
    worker: PhantomXWorkerCfg = MISSING
    runner: RslRlRunnerCfg = MISSING


def load_phantomx_training_config(path: Path | None = None) -> PhantomXTrainingConfig:
    """Load training YAML through configspec and assemble runtime partitions."""

    config_path = DEFAULT_PHANTOMX_TRAINING_CONFIG if path is None else path
    try:
        text = config_path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise ConfigError(
            f"PhantomX training config not found at {config_path}",
            code="CONFIG_NOT_FOUND",
            path=str(config_path),
        ) from exc
    return parse_phantomx_training_config(text, source=str(config_path))


def load_phantomx_terrain_config(path: Path | None = None) -> Mapping[str, object]:
    """Load the continuous heightfield profile owned by its terrain Task."""

    return _load_terrain_config(DEFAULT_PHANTOMX_TERRAIN_CONFIG if path is None else path)


def load_phantomx_discrete_terrain_config(path: Path | None = None) -> Mapping[str, object]:
    """Load the discrete curriculum profile owned by its terrain Task."""

    return _load_terrain_config(
        DEFAULT_PHANTOMX_DISCRETE_TERRAIN_CONFIG if path is None else path
    )


def _load_terrain_config(config_path: Path) -> Mapping[str, object]:
    """Load one Python-owned terrain profile through configspec."""

    try:
        text = config_path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise ConfigError(
            f"PhantomX terrain config not found at {config_path}",
            code="CONFIG_NOT_FOUND",
            path=str(config_path),
        ) from exc
    try:
        document = load_unique_yaml(text)
    except yaml.YAMLError as exc:
        raise ConfigError(
            "PhantomX terrain config is not valid YAML",
            code="CONFIG_INVALID_YAML",
            path=str(config_path),
        ) from exc
    if not isinstance(document, Mapping):
        raise ConfigError(
            "expected a mapping with string keys",
            code="CONFIG_NOT_MAPPING",
            path=str(config_path),
        )
    return parse_terrain_config(dict(document))


def parse_phantomx_training_config(text: str, *, source: str = "<string>") -> PhantomXTrainingConfig:
    """Parse PhantomX training YAML with ``from_mapping`` validation."""

    try:
        document = load_unique_yaml(text)
    except yaml.YAMLError as exc:
        raise ConfigError(
            "PhantomX training config is not valid YAML",
            code="CONFIG_INVALID_YAML",
            path=source,
        ) from exc
    if not isinstance(document, Mapping):
        raise ConfigError(
            "expected a mapping with string keys",
            code="CONFIG_NOT_MAPPING",
            path=source,
        )
    prepared = _prepare_phantomx_document(dict(document))
    cfg = from_mapping(PhantomXTrainingCfg, prepared)
    return _assemble_phantomx_training_config(cfg)


def _prepare_phantomx_document(document: dict[str, object]) -> dict[str, object]:
    """Rewrite dotted environment scalar keys into configspec field names."""

    worker = document.get("worker")
    if not isinstance(worker, dict):
        return document
    environment = worker.get("environment")
    if not isinstance(environment, dict):
        return document
    scalars = environment.get("scalars")
    if not isinstance(scalars, dict):
        return document
    rewritten: dict[str, object] = {}
    for key, value in scalars.items():
        if not isinstance(key, str):
            rewritten[key] = value
            continue
        if key in _ENV_SCALAR_YAML_TO_FIELD:
            rewritten[_ENV_SCALAR_YAML_TO_FIELD[key]] = value
        else:
            rewritten[key] = value
    environment["scalars"] = rewritten
    return document


def _assemble_phantomx_training_config(cfg: PhantomXTrainingCfg) -> PhantomXTrainingConfig:
    """Apply PhantomX invariants and build runtime Worker/Task/runner objects."""

    _validate_phantomx_invariants(cfg)
    if cfg.robot.asset_path != PHANTOMX_ASSET_PATH:
        raise ConfigError(
            f"robot.asset_path must equal {PHANTOMX_ASSET_PATH!r}",
            code="CONFIG_OUT_OF_RANGE",
            path="robot.asset_path",
        )
    robot_asset = get_robot_asset(cfg.robot.config_path)
    if cfg.robot.asset_path != robot_asset.asset_path:
        raise ConfigError(
            f"robot.asset_path must equal {robot_asset.asset_path!r} for {cfg.robot.config_path!r}",
            code="CONFIG_OUT_OF_RANGE",
            path="robot.asset_path",
        )
    robot_config = robot_asset.to_robot_config()
    _validate_obs_group_names(cfg.runner.parameters.obs_groups)
    validate_decimation_int32_range(cfg.worker.decimation)

    scalars = cfg.worker.environment.scalars
    if not float(scalars.columns).is_integer():
        raise ConfigError(
            "environment.columns must be an integer value",
            code="CONFIG_OUT_OF_RANGE",
            path="worker.environment.scalars.columns",
        )
    environment_config = {
        _ENV_SCALAR_FIELD_TO_YAML[name]: getattr(scalars, name)
        for name in (
            "spacing_x_m",
            "spacing_y_m",
            "columns",
            "origin_x_m",
            "origin_y_m",
            "trace_start_z_m",
            "trace_depth_m",
        )
    }
    task_cfg = cfg.task
    task = PhantomXTaskConfig(
        max_episode_duration_s=task_cfg.max_episode_duration_s,
        reference_dt_s=task_cfg.reference_dt_s,
        slot_fault_reward=task_cfg.slot_fault_reward,
        action_clip=task_cfg.action_clip,
        velocity_tracking_std=task_cfg.velocity_tracking_std,
        yaw_rate_tracking_std=task_cfg.yaw_rate_tracking_std,
        heading_control_stiffness=task_cfg.heading_control_stiffness,
        max_yaw_rate=task_cfg.max_yaw_rate,
        turn_start_distance=task_cfg.turn_start_distance,
        turn_completion_tolerance=task_cfg.turn_completion_tolerance,
        command=PhantomXCommandConfig(
            initial_speed_min=task_cfg.command.initial_speed_min,
            initial_speed_max=task_cfg.command.initial_speed_max,
            post_turn_speed_min=task_cfg.command.post_turn_speed_min,
            post_turn_speed_max=task_cfg.command.post_turn_speed_max,
            heading_delta_min=task_cfg.command.heading_delta_min,
            heading_delta_max=task_cfg.command.heading_delta_max,
            resampling_time_min_s=task_cfg.command.resampling_time_min_s,
            resampling_time_max_s=task_cfg.command.resampling_time_max_s,
            standing_probability=task_cfg.command.standing_probability,
        ),
        curriculum=PhantomXCurriculumConfig(
            window_episodes=task_cfg.curriculum.window_episodes,
            minimum_episodes=task_cfg.curriculum.minimum_episodes,
            promotion_success_rate=task_cfg.curriculum.promotion_success_rate,
            velocity_error_threshold=task_cfg.curriculum.velocity_error_threshold,
        ),
        moving_command_threshold=task_cfg.moving_command_threshold,
        body_clearance_penalty_threshold=task_cfg.body_clearance_penalty_threshold,
        base_contact_force_threshold=task_cfg.base_contact_force_threshold,
        observation_noise=PhantomXObservationNoiseConfig(
            lin_vel_b=task_cfg.observation_noise.lin_vel_b,
            ang_vel_b=task_cfg.observation_noise.ang_vel_b,
            gravity_b=task_cfg.observation_noise.gravity_b,
            joint_pos_rel=task_cfg.observation_noise.joint_pos_rel,
            joint_vel=task_cfg.observation_noise.joint_vel,
        ),
        events=PhantomXTrainingEventConfig(
            static_friction_min=task_cfg.events.static_friction_min,
            static_friction_max=task_cfg.events.static_friction_max,
            dynamic_friction_min=task_cfg.events.dynamic_friction_min,
            dynamic_friction_max=task_cfg.events.dynamic_friction_max,
            push_velocity_min_mps=task_cfg.events.push_velocity_min_mps,
            push_velocity_max_mps=task_cfg.events.push_velocity_max_mps,
            push_interval_min_s=task_cfg.events.push_interval_min_s,
            push_interval_max_s=task_cfg.events.push_interval_max_s,
        ),
        linear_velocity_tracking_weight=task_cfg.reward.linear_velocity_tracking,
        linear_velocity_progress_weight=task_cfg.reward.linear_velocity_progress,
        yaw_rate_tracking_weight=task_cfg.reward.yaw_rate_tracking,
        yaw_rate_progress_weight=task_cfg.reward.yaw_rate_progress,
        vertical_velocity_weight=task_cfg.reward.vertical_velocity,
        body_angular_velocity_weight=task_cfg.reward.body_angular_velocity,
        upright_weight=task_cfg.reward.upright,
        body_clearance_weight=task_cfg.reward.body_clearance,
        action_rate_weight=task_cfg.reward.action_rate,
        joint_velocity_penalty=task_cfg.reward.joint_velocity,
        fall_penalty=task_cfg.reward.fall,
    )
    worker = WorkerConfig(
        slot_count=cfg.worker.slot_count,
        physics_dt=cfg.worker.physics_dt,
        decimation=cfg.worker.decimation,
        environment_id=PHANTOMX_ENVIRONMENT_ID,
        robot_id=PHANTOMX_ROBOT_ID,
        environment_config=environment_config,
        robot_asset_path=cfg.robot.asset_path,
        robot_config_path=Path(cfg.robot.config_path),
        robot_semantics=robot_config,
        robot_config={},
        run_seed=cfg.worker.run_seed,
    )
    runner = RslRlRunnerConfig(
        rollout_length=cfg.runner.rollout_length,
        max_iterations=cfg.runner.max_iterations,
        device=cfg.runner.device,
        parameters=to_dict(cfg.runner.parameters),
    )
    return PhantomXTrainingConfig(
        cfg.robot.asset_path,
        Path(cfg.robot.config_path),
        robot_config,
        task,
        worker,
        runner,
    )


def _validate_phantomx_invariants(cfg: PhantomXTrainingCfg) -> None:
    """Enforce command/curriculum cross-field rules after configspec parse."""

    command = cfg.task.command
    if (
        command.initial_speed_min > command.initial_speed_max
        or command.post_turn_speed_min > command.post_turn_speed_max
        or command.heading_delta_min > command.heading_delta_max
        or command.resampling_time_min_s > command.resampling_time_max_s
    ):
        raise ConfigError(
            "PhantomX command ranges are invalid",
            code="CONFIG_OUT_OF_RANGE",
            path="task.command",
        )
    events = cfg.task.events
    if (
        events.static_friction_min > events.static_friction_max
        or events.dynamic_friction_min > events.dynamic_friction_max
        or events.push_velocity_min_mps > events.push_velocity_max_mps
        or events.push_interval_min_s > events.push_interval_max_s
    ):
        raise ConfigError(
            "PhantomX training event ranges are invalid",
            code="CONFIG_OUT_OF_RANGE",
            path="task.events",
        )
    curriculum = cfg.task.curriculum
    if curriculum.minimum_episodes > curriculum.window_episodes:
        raise ConfigError(
            "minimum_episodes must not exceed window_episodes",
            code="CONFIG_OUT_OF_RANGE",
            path="task.curriculum.minimum_episodes",
        )


def _validate_obs_group_names(groups: RslRlObsGroupsCfg) -> None:
    """Reject blank observation group names that configspec cannot yet constrain."""

    for group_name in ("actor", "critic"):
        values = getattr(groups, group_name)
        for index, item in enumerate(values):
            if not item:
                raise ConfigError(
                    "observation groups must be non-empty string lists",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"runner.parameters.obs_groups.{group_name}[{index}]",
                )


__all__ = [
    "DEFAULT_PHANTOMX_TERRAIN_CONFIG",
    "DEFAULT_PHANTOMX_DISCRETE_TERRAIN_CONFIG",
    "DEFAULT_PHANTOMX_TRAINING_CONFIG",
    "PHANTOMX_COMMAND_FIELDS",
    "PHANTOMX_FEET",
    "PHANTOMX_ENVIRONMENT_ID",
    "PHANTOMX_DISCRETE_TERRAIN_ENVIRONMENT_ID",
    "PHANTOMX_JOINTS",
    "PHANTOMX_ROBOT_ID",
    "PHANTOMX_SLOT_ISOLATED_ENVIRONMENT_ID",
    "PHANTOMX_TASK_ID",
    "PHANTOMX_TERRAIN_ENVIRONMENT_ID",
    "PHANTOMX_TERRAIN_TASK_ID",
    "PHANTOMX_DISCRETE_TERRAIN_TASK_ID",
    "PHANTOMX_TASK_VERSION",
    "PHANTOMX_ASSET_PATH",
    "PhantomXCommandConfig",
    "PhantomXCurriculumConfig",
    "PhantomXTaskConfig",
    "PhantomXTrainingCfg",
    "PhantomXTrainingConfig",
    "load_phantomx_terrain_config",
    "load_phantomx_discrete_terrain_config",
    "load_phantomx_training_config",
    "parse_phantomx_training_config",
]

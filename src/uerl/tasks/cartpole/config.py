"""Define the typed Cart-Pole task parameters and YAML configspec loading."""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

import yaml

from ...assets.robots import get_robot_asset
from ...core.config.configspec import MISSING, configspec, from_mapping, spec_field, to_dict
from ...core.config.models import DirectTaskConfig, RslRlRunnerConfig, WorkerConfig, validate_decimation_int32_range
from ...core.config.paths import repository_config_root
from ...core.config.robot import (
    RobotConfig,
    RobotSpec,
    RobotTopology,
    merge_robot_spec,
)
from ...core.config.terrain_spec import parse_terrain_config
from ...core.config.training_spec import (
    RobotRefCfg,
    RslRlObsGroupsCfg,
    RslRlRunnerCfg,
    RslRlRunnerParametersCfg,
)
from ...core.config.yaml_loader import load_unique_yaml
from ...errors import ConfigError

CARTPOLE_TASK_ID = "UERL-CartPole-Direct-v0"
CARTPOLE_TASK_VERSION = "1.0.0"
CARTPOLE_ENVIRONMENT_ID = "uerl.environment.shared_world"
CARTPOLE_ROBOT_ID = "uerl.robot.skeletal_mesh"
CARTPOLE_PHYSICS_DT = 1.0 / 120.0
CARTPOLE_DECIMATION = 2
CARTPOLE_CONTROL_DT = CARTPOLE_PHYSICS_DT * CARTPOLE_DECIMATION
CARTPOLE_MAX_EPISODE_STEPS = 300
CARTPOLE_TERRAIN_CONFIG_PATH = "environments/terrains/cartpole/flat.yaml"
DEFAULT_CARTPOLE_TERRAIN_CONFIG = (
    repository_config_root() / "environments" / "terrains" / "cartpole" / "flat.yaml"
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
_ENV_SCALAR_NAMES = (
    "spacing_x_m",
    "spacing_y_m",
    "columns",
    "origin_x_m",
    "origin_y_m",
    "trace_start_z_m",
    "trace_depth_m",
)


@dataclass(frozen=True, slots=True)
class CartPoleTaskConfig(DirectTaskConfig):
    """Store the typed Cart-Pole action, termination, timeout, and reward settings.

    The four raw State fields and one Physical Command field match the UE
    `uerl.robot.skeletal_mesh` descriptor. Reward weights follow the fixed
    Direct Cart-Pole reference in the read-only Isaac Lab submodule, while the
    field names and physical execution remain owned by this repository.
    """

    state_requirements: tuple[str, ...] = ()
    action_schema: tuple[str, ...] = ()
    max_episode_steps: int = CARTPOLE_MAX_EPISODE_STEPS
    slot_fault_reward: float = -2.0
    max_cart_position: float = 3.0
    pole_angle_limit: float = math.pi / 2
    action_clip: float = 1.0
    rew_scale_alive: float = 1.0
    rew_scale_terminated: float = -2.0
    rew_scale_pole_pos: float = -1.0
    rew_scale_cart_vel: float = -0.01
    rew_scale_pole_vel: float = -0.005

    @classmethod
    def from_direct(cls, config: DirectTaskConfig) -> CartPoleTaskConfig:
        """Convert a generic DirectTask Config to Cart-Pole defaults.

        Args:
            config: A typed task Config supplied through the Registry boundary.
                Existing Cart-Pole-specific fields are preserved when the value
                is already a ``CartPoleTaskConfig``.

        Returns:
            A Cart-Pole Config with the generic timeout and fault reward copied
            from ``config`` and all omitted Cart-Pole parameters at defaults.
        """

        if isinstance(config, cls):
            return config
        return cls(
            parameters=config.parameters,
            max_episode_steps=config.max_episode_steps,
            slot_fault_reward=config.slot_fault_reward,
        )


DEFAULT_CARTPOLE_TRAINING_CONFIG = repository_config_root() / "tasks" / "cartpole" / "training.yaml"


@dataclass(frozen=True, slots=True)
class CartPoleTrainingConfig:
    """Hold the asset-library Cart-Pole Robot and YAML task partitions."""

    robot_asset_path: str
    robot_config_path: Path
    robot_config: RobotConfig
    task: CartPoleTaskConfig
    worker: WorkerConfig
    runner: RslRlRunnerConfig

    def resolve_robot_spec(self, topology: Mapping[str, object]) -> RobotSpec:
        """Bind the configured Robot semantics to one UE topology response."""
        return merge_robot_spec(self.robot_config, RobotTopology.from_response(topology))


@configspec
class CartPoleIdentityCfg:
    """YAML identity partition with Cart-Pole registration constants."""

    task_id: Literal["UERL-CartPole-Direct-v0"] = MISSING
    task_version: Literal["1.0.0"] = MISSING
    environment_id: Literal["uerl.environment.shared_world"] = MISSING
    robot_id: Literal["uerl.robot.skeletal_mesh"] = MISSING


@configspec
class CartPoleRewardCfg:
    """Reward scale partition under ``task.reward``."""

    alive: float = spec_field(MISSING, finite=True)
    terminated: float = spec_field(MISSING, finite=True)
    pole_position: float = spec_field(MISSING, finite=True)
    cart_velocity: float = spec_field(MISSING, finite=True)
    pole_velocity: float = spec_field(MISSING, finite=True)


@configspec
class CartPoleTaskCfg:
    """YAML task partition for Direct Cart-Pole."""

    max_episode_steps: int = spec_field(MISSING, gt=0, integral=True)
    slot_fault_reward: float = spec_field(MISSING, finite=True)
    max_cart_position: float = spec_field(MISSING, gt=0.0, finite=True)
    pole_angle_limit: float = spec_field(MISSING, gt=0.0, finite=True)
    action_clip: float = spec_field(MISSING, gt=0.0, finite=True)
    reward: CartPoleRewardCfg = MISSING


@configspec
class CartPoleEnvironmentScalarsCfg:
    """Shared-world placement scalars after dotted YAML keys are rewritten."""

    spacing_x_m: float = spec_field(MISSING, gt=0.0, finite=True)
    spacing_y_m: float = spec_field(MISSING, gt=0.0, finite=True)
    columns: float = spec_field(MISSING, ge=1.0, le=65536.0, finite=True)
    origin_x_m: float = spec_field(MISSING, finite=True)
    origin_y_m: float = spec_field(MISSING, finite=True)
    trace_start_z_m: float = spec_field(MISSING, finite=True)
    trace_depth_m: float = spec_field(MISSING, gt=0.0, finite=True)


@configspec
class CartPoleEnvironmentCfg:
    """Worker environment identity and scalars."""

    id: Literal["uerl.environment.shared_world"] = MISSING
    scalars: CartPoleEnvironmentScalarsCfg = MISSING


@configspec
class CartPoleWorkerCfg:
    """YAML worker partition projected into ``WorkerConfig``."""

    slot_count: int = spec_field(MISSING, gt=0, le=65536, integral=True)
    physics_dt: float = spec_field(MISSING, gt=0.0, le=1.0, finite=True)
    decimation: tuple[int, int] = spec_field(MISSING, item_gt=0, ordered=True)
    environment: CartPoleEnvironmentCfg = MISSING
    run_seed: int = spec_field(MISSING, ge=0, le=2**64 - 1, integral=True)  # type: ignore[misc]


@configspec
class CartPoleTrainingCfg:
    """Top-level Cart-Pole training YAML document."""

    identity: CartPoleIdentityCfg = MISSING
    robot: RobotRefCfg = MISSING
    task: CartPoleTaskCfg = MISSING
    worker: CartPoleWorkerCfg = MISSING
    runner: RslRlRunnerCfg = MISSING


def load_cartpole_training_config(path: Path | None = None) -> CartPoleTrainingConfig:
    """Load training YAML through configspec and assemble runtime partitions."""

    config_path = DEFAULT_CARTPOLE_TRAINING_CONFIG if path is None else path
    try:
        text = config_path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise ConfigError(
            f"Cart-Pole training config not found at {config_path}",
            code="CONFIG_NOT_FOUND",
            path=str(config_path),
        ) from exc
    return parse_cartpole_training_config(text, source=str(config_path))


def parse_cartpole_training_config(
    text: str,
    *,
    source: str = "<string>",
) -> CartPoleTrainingConfig:
    """Parse Cart-Pole training YAML with ``from_mapping`` validation."""

    try:
        document = load_unique_yaml(text)
    except yaml.YAMLError as exc:
        raise ConfigError(
            "Cart-Pole training config is not valid YAML",
            code="CONFIG_INVALID_YAML",
            path=source,
        ) from exc
    if not isinstance(document, Mapping):
        raise ConfigError(
            "expected a mapping with string keys",
            code="CONFIG_NOT_MAPPING",
            path=source,
        )
    prepared = _prepare_cartpole_document(dict(document))
    cfg = from_mapping(CartPoleTrainingCfg, prepared)
    return _assemble_cartpole_training_config(cfg)


def _prepare_cartpole_document(document: dict[str, object]) -> dict[str, object]:
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


def _assemble_cartpole_training_config(cfg: CartPoleTrainingCfg) -> CartPoleTrainingConfig:
    """Apply Cart-Pole invariants and build runtime Worker/Task/runner objects."""

    _validate_cartpole_invariants(cfg)
    if not cfg.robot.asset_path.startswith("/Game/"):
        raise ConfigError(
            "robot.asset_path must be a UE object path under /Game/",
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
    if len(robot_config.actuators) != 1:
        raise ConfigError(
            "Cart-Pole requires exactly one Robot actuator",
            code="CONFIG_OUT_OF_RANGE",
            path="robot.actuators",
        )
    _validate_obs_group_names(cfg.runner.parameters.obs_groups)
    validate_decimation_int32_range(cfg.worker.decimation)

    terrain_config = load_cartpole_terrain_config()
    environment_config = _shared_world_layout(
        slot_count=cfg.worker.slot_count,
        columns=int(cfg.worker.environment.scalars.columns),
        terrain_config=terrain_config,
        trace_start_z_m=cfg.worker.environment.scalars.trace_start_z_m,
        trace_depth_m=cfg.worker.environment.scalars.trace_depth_m,
    )
    task = CartPoleTaskConfig(
        max_episode_steps=cfg.task.max_episode_steps,
        slot_fault_reward=cfg.task.slot_fault_reward,
        max_cart_position=cfg.task.max_cart_position,
        pole_angle_limit=cfg.task.pole_angle_limit,
        action_clip=cfg.task.action_clip,
        rew_scale_alive=cfg.task.reward.alive,
        rew_scale_terminated=cfg.task.reward.terminated,
        rew_scale_pole_pos=cfg.task.reward.pole_position,
        rew_scale_cart_vel=cfg.task.reward.cart_velocity,
        rew_scale_pole_vel=cfg.task.reward.pole_velocity,
    )
    worker = WorkerConfig(
        slot_count=cfg.worker.slot_count,
        physics_dt=cfg.worker.physics_dt,
        decimation=cfg.worker.decimation,
        environment_id=CARTPOLE_ENVIRONMENT_ID,
        robot_id=CARTPOLE_ROBOT_ID,
        environment_config=environment_config,
        robot_asset_path=cfg.robot.asset_path,
        robot_config_path=Path(cfg.robot.config_path),
        robot_semantics=robot_config,
        robot_config={},
        run_seed=cfg.worker.run_seed,
        terrain_config=terrain_config,
        terrain_config_path=Path(CARTPOLE_TERRAIN_CONFIG_PATH),
    )
    parameters = to_dict(cfg.runner.parameters)
    runner = RslRlRunnerConfig(
        rollout_length=cfg.runner.rollout_length,
        max_iterations=cfg.runner.max_iterations,
        device=cfg.runner.device,
        parameters=parameters,
    )
    return CartPoleTrainingConfig(
        robot_asset_path=cfg.robot.asset_path,
        robot_config_path=Path(cfg.robot.config_path),
        robot_config=robot_config,
        task=task,
        worker=worker,
        runner=runner,
    )


def load_cartpole_terrain_config(path: Path | None = None) -> Mapping[str, object]:
    """Load the flat shared-world plane under CartPole slots."""

    config_path = DEFAULT_CARTPOLE_TERRAIN_CONFIG if path is None else path
    try:
        text = config_path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise ConfigError(
            f"Cart-Pole terrain config not found at {config_path}",
            code="CONFIG_NOT_FOUND",
            path=str(config_path),
        ) from exc
    try:
        document = load_unique_yaml(text)
    except yaml.YAMLError as exc:
        raise ConfigError(
            "Cart-Pole terrain config is not valid YAML",
            code="CONFIG_INVALID_YAML",
            path=str(config_path),
        ) from exc
    if not isinstance(document, Mapping):
        raise ConfigError(
            "expected a mapping with string keys",
            code="CONFIG_NOT_MAPPING",
            path=str(config_path),
        )
    return parse_terrain_config(dict(document), path=str(config_path))


def _shared_world_layout(
    *,
    slot_count: int,
    columns: int,
    terrain_config: Mapping[str, object],
    trace_start_z_m: float,
    trace_depth_m: float,
) -> dict[str, object]:
    """Centre a slot lattice on the generated plane patch (PhantomX shared layout)."""

    from math import ceil
    from typing import cast

    if columns < 2:
        raise ConfigError(
            "shared_world columns must be at least 2 for a spawn lattice",
            code="CONFIG_OUT_OF_RANGE",
            path="worker.environment.scalars.columns",
        )
    rows = ceil(slot_count / columns)
    if rows < 2:
        raise ConfigError(
            "shared_world needs at least two rows for the configured slot_count",
            code="CONFIG_OUT_OF_RANGE",
            path="worker.slot_count",
        )
    cell_size = cast(tuple[float, float] | list[float], terrain_config["cell_size"])
    if not isinstance(cell_size, (list, tuple)) or len(cell_size) != 2:
        raise ConfigError(
            "terrain cell_size must be a length-2 sequence",
            code="CONFIG_TYPE_MISMATCH",
            path="terrain.cell_size",
        )
    border_width = float(cast(float, terrain_config["border_width"]))
    slot_margin = 0.5
    usable_x = float(cell_size[0]) - 2.0 * border_width - 2.0 * slot_margin
    usable_y = float(cell_size[1]) - 2.0 * border_width - 2.0 * slot_margin
    if usable_x <= 0.0 or usable_y <= 0.0:
        raise ConfigError(
            "Cart-Pole plane cell_size is too small for the spawn lattice",
            code="CONFIG_OUT_OF_RANGE",
            path="terrain.cell_size",
        )
    return {
        "environment.spacing_x_m": usable_x / float(columns - 1),
        "environment.spacing_y_m": usable_y / float(rows - 1),
        "environment.columns": float(columns),
        "environment.origin_x_m": -0.5 * usable_x,
        "environment.origin_y_m": -0.5 * usable_y,
        "environment.trace_start_z_m": float(trace_start_z_m),
        "environment.trace_depth_m": float(trace_depth_m),
    }


def _validate_cartpole_invariants(cfg: CartPoleTrainingCfg) -> None:
    """Enforce Cart-Pole fixed step baseline and shared-world columns integrity."""

    if cfg.task.max_episode_steps != CARTPOLE_MAX_EPISODE_STEPS:
        raise ConfigError(
            "Cart-Pole requires a five-second episode at 60 Hz",
            code="CONFIG_OUT_OF_RANGE",
            path="task.max_episode_steps",
        )
    if not math.isclose(cfg.worker.physics_dt, CARTPOLE_PHYSICS_DT, rel_tol=0.0, abs_tol=1.0e-12):
        raise ConfigError(
            "Cart-Pole requires physics_dt = 1/120",
            code="CONFIG_OUT_OF_RANGE",
            path="worker.physics_dt",
        )
    if cfg.worker.decimation != (CARTPOLE_DECIMATION, CARTPOLE_DECIMATION):
        raise ConfigError(
            "Cart-Pole requires decimation = [2, 2]",
            code="CONFIG_OUT_OF_RANGE",
            path="worker.decimation",
        )
    columns = cfg.worker.environment.scalars.columns
    if not float(columns).is_integer():
        raise ConfigError(
            "environment.columns must be an integer count",
            code="CONFIG_OUT_OF_RANGE",
            path="worker.environment.scalars.columns",
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
    "CARTPOLE_ENVIRONMENT_ID",
    "CARTPOLE_CONTROL_DT",
    "CARTPOLE_DECIMATION",
    "CARTPOLE_MAX_EPISODE_STEPS",
    "CARTPOLE_PHYSICS_DT",
    "CARTPOLE_ROBOT_ID",
    "CARTPOLE_TASK_ID",
    "CARTPOLE_TASK_VERSION",
    "CARTPOLE_TERRAIN_CONFIG_PATH",
    "DEFAULT_CARTPOLE_TERRAIN_CONFIG",
    "DEFAULT_CARTPOLE_TRAINING_CONFIG",
    "CartPoleTaskCfg",
    "CartPoleTaskConfig",
    "CartPoleTrainingCfg",
    "CartPoleTrainingConfig",
    "RobotRefCfg",
    "RslRlObsGroupsCfg",
    "RslRlRunnerCfg",
    "RslRlRunnerParametersCfg",
    "load_cartpole_terrain_config",
    "load_cartpole_training_config",
    "parse_cartpole_training_config",
]

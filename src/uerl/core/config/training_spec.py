"""Shared training YAML configspec partitions reused by task loaders."""

from __future__ import annotations

from typing import Literal

from .configspec import MISSING, configspec, spec_field


@configspec
class RobotRefCfg:
    """UE asset path plus stable Python Robot asset declaration key."""

    asset_path: str = spec_field(MISSING, min_length=1)
    config_path: str = spec_field(MISSING, min_length=1)


@configspec
class RslRlObsGroupsCfg:
    """Actor/critic observation group names for RSL-RL."""

    actor: tuple[str, ...] = spec_field(MISSING, min_length=1)
    critic: tuple[str, ...] = spec_field(MISSING, min_length=1)


@configspec
class RslRlRunnerParametersCfg:
    """Strict runner.parameters partition shared by training YAML loaders."""

    experiment_name: str = spec_field(MISSING, min_length=1)
    run_name: str = spec_field(MISSING, min_length=1)
    hidden_dims: tuple[int, ...] = spec_field(MISSING, min_length=1, item_gt=0)
    activation: str = spec_field(MISSING, min_length=1)
    learning_rate: float = spec_field(MISSING, gt=0.0, finite=True)
    gamma: float = spec_field(MISSING, ge=0.0, le=1.0, finite=True)
    lam: float = spec_field(MISSING, ge=0.0, le=1.0, finite=True)
    save_interval: int = spec_field(MISSING, gt=0, integral=True)
    init_std: float = spec_field(MISSING, gt=0.0, finite=True)
    num_learning_epochs: int = spec_field(MISSING, gt=0, integral=True)
    num_mini_batches: int = spec_field(MISSING, gt=0, integral=True)
    clip_param: float = spec_field(MISSING, gt=0.0, finite=True)
    entropy_coef: float = spec_field(MISSING, finite=True)
    obs_normalization: bool = MISSING
    obs_groups: RslRlObsGroupsCfg = MISSING
    actor_class_name: str = spec_field(MISSING, min_length=1)  # type: ignore[misc]
    critic_class_name: str = spec_field(MISSING, min_length=1)  # type: ignore[misc]
    algorithm_class_name: str = spec_field(MISSING, min_length=1)  # type: ignore[misc]
    check_for_nan: bool = MISSING
    rnd_cfg: Literal[None] = MISSING
    symmetry_cfg: Literal[None] = MISSING


@configspec
class RslRlRunnerCfg:
    """YAML runner partition assembled into ``RslRlRunnerConfig``."""

    rollout_length: int = spec_field(MISSING, gt=0, integral=True)
    max_iterations: int = spec_field(MISSING, gt=0, integral=True)
    device: str = spec_field(MISSING, min_length=1)
    parameters: RslRlRunnerParametersCfg = MISSING


__all__ = [
    "RobotRefCfg",
    "RslRlObsGroupsCfg",
    "RslRlRunnerCfg",
    "RslRlRunnerParametersCfg",
]

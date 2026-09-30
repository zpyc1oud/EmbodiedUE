"""Registry for the built-in Python-owned Robot asset declarations."""

from __future__ import annotations

from types import MappingProxyType

from ...core.config.robot import RobotConfig
from ...errors import ConfigError
from .base import (
    ActuatorGroupCfg,
    ObsSelectorCfg,
    ResetTargetCfg,
    RobotAssetCfg,
    RobotInitStateCfg,
    replace_actuators,
)
from .cartpole import CARTPOLE_ASSET_PATH, CARTPOLE_ASSET_REF, CARTPOLE_CFG
from .phantomx import PHANTOMX_ASSET_PATH, PHANTOMX_ASSET_REF, PHANTOMX_CFG

ROBOT_ASSETS = MappingProxyType(
    {
        CARTPOLE_ASSET_REF: CARTPOLE_CFG,
        PHANTOMX_ASSET_REF: PHANTOMX_CFG,
    }
)


def get_robot_asset(reference: str) -> RobotAssetCfg:
    """Resolve one stable declaration key without touching the filesystem."""

    asset = ROBOT_ASSETS.get(reference)
    if asset is None:
        available = ", ".join(sorted(ROBOT_ASSETS))
        raise ConfigError(
            f"unknown robot asset declaration {reference!r}; available: {available}",
            code="ROBOT_ASSET_NOT_FOUND",
            path="robot.config_path",
        )
    return asset


def load_robot_asset(reference: str) -> RobotConfig:
    """Resolve and materialise a built-in Robot declaration."""

    return get_robot_asset(reference).to_robot_config()


__all__ = [
    "ActuatorGroupCfg",
    "CARTPOLE_ASSET_PATH",
    "CARTPOLE_ASSET_REF",
    "CARTPOLE_CFG",
    "ObsSelectorCfg",
    "PHANTOMX_ASSET_PATH",
    "PHANTOMX_ASSET_REF",
    "PHANTOMX_CFG",
    "ResetTargetCfg",
    "RobotAssetCfg",
    "RobotInitStateCfg",
    "ROBOT_ASSETS",
    "get_robot_asset",
    "load_robot_asset",
    "replace_actuators",
]

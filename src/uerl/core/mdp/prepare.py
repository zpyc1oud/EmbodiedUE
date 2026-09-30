"""Shared assemble helpers for MDP managers."""

from __future__ import annotations

from typing import Any

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec


def prepare_term_params(params: dict[str, Any], spec: RobotSpec) -> dict[str, Any]:
    """Resolve ``RobotEntityCfg`` values inside term params against ``spec``."""

    prepared: dict[str, Any] = {}
    for key, value in params.items():
        if isinstance(value, RobotEntityCfg):
            if not value.resolved:
                value.resolve(spec)
            prepared[key] = value
        else:
            prepared[key] = value
    return prepared


__all__ = ["prepare_term_params"]

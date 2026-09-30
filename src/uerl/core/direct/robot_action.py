"""Resolve policy actions into one indexed generic-robot target field."""

from __future__ import annotations

from collections.abc import Mapping
from types import MappingProxyType
from typing import Any

import torch

from ..config.robot import RobotSpec
from .types import PhysicalCommandBatch

ROBOT_ACTUATOR_TARGET_FIELD = "robot.actuator.target"


def robot_actuator_action_schema(spec: RobotSpec) -> tuple[Mapping[str, Any], ...]:
    """Build the one-vector Action descriptor with column metadata."""
    units = {actuator.target_unit for actuator in spec.actuators}
    unit = next(iter(units)) if len(units) == 1 else "mixed"
    columns = tuple(
        MappingProxyType(
            {
                "index": actuator.index,
                "joint": actuator.joint,
                "joint_index": actuator.joint_index,
                "coordinate_type": actuator.coordinate_type,
                "unit": actuator.target_unit,
                "target_mode": actuator.target_mode,
            }
        )
        for actuator in spec.actuators
    )
    return (
        MappingProxyType(
            {
                "name": ROBOT_ACTUATOR_TARGET_FIELD,
                "dtype": "float32",
                "shape": (len(spec.actuators),),
                "unit": unit,
                "frame": "slot/robot",
                "semantic": "actuator_target",
                "source": "uerl.robot",
                "extensions": MappingProxyType({"columns": columns}),
            }
        ),
    )


def resolve_robot_actions(spec: RobotSpec, policy_actions: torch.Tensor) -> PhysicalCommandBatch:
    """Resolve policy outputs into the indexed physical-unit target vector."""
    actuator_count = len(spec.actuators)
    if policy_actions.ndim != 2 or policy_actions.shape[1] != actuator_count:
        raise ValueError(f"Robot policy actions must have shape (num_rows, {actuator_count})")

    actions = policy_actions.to(dtype=torch.float32)
    defaults = torch.tensor(
        [actuator.default_pos for actuator in spec.actuators],
        dtype=torch.float32,
        device=actions.device,
    )
    scales = torch.tensor(
        [actuator.action_scale for actuator in spec.actuators],
        dtype=torch.float32,
        device=actions.device,
    )
    stiffness = torch.tensor(
        [actuator.stiffness for actuator in spec.actuators],
        dtype=torch.float32,
        device=actions.device,
    )
    scaled = actions * scales
    targets = torch.where(stiffness > 0.0, defaults + scaled, scaled)
    return PhysicalCommandBatch({ROBOT_ACTUATOR_TARGET_FIELD: targets})


__all__ = ["ROBOT_ACTUATOR_TARGET_FIELD", "resolve_robot_actions", "robot_actuator_action_schema"]

"""Task-agnostic metric helpers for training logs (not deploy/export)."""

from __future__ import annotations

from collections.abc import Mapping
from typing import cast

import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.direct.types import StepContext
from uerl.errors import ConfigError


def body_pose(state: Mapping[str, torch.Tensor], entity: RobotEntityCfg) -> torch.Tensor:
    return _concat_fields(state, entity.body_field_names, "body_pose")


def body_lin_vel_b(ctx: StepContext, *, entity: RobotEntityCfg) -> torch.Tensor:
    """Body-frame linear velocity (Nx3) from world velocity + pose quaternion."""

    pose = body_pose(ctx.transition_state, entity)
    world_vel = _concat_fields(
        ctx.transition_state, entity.body_field_names, "body_linear_velocity"
    )
    return _quat_rotate_inverse(pose[:, 3:7], world_vel[:, :3])


def body_ang_vel_b(ctx: StepContext, *, entity: RobotEntityCfg) -> torch.Tensor:
    """Body-frame angular velocity (Nx3)."""

    pose = body_pose(ctx.transition_state, entity)
    world_ang = _concat_fields(
        ctx.transition_state, entity.body_field_names, "body_angular_velocity"
    )
    return _quat_rotate_inverse(pose[:, 3:7], world_ang[:, :3])


def upright_from_pose(pose: torch.Tensor) -> torch.Tensor:
    """Cosine of tilt from world-up using pose quaternion xyzw."""

    return 1.0 - 2.0 * (pose[:, 3].square() + pose[:, 4].square())


def planar_displacement(pose: torch.Tensor) -> torch.Tensor:
    return cast(torch.Tensor, torch.linalg.vector_norm(pose[:, :2], dim=1))


def body_scalar_field(
    state: Mapping[str, torch.Tensor],
    entity: RobotEntityCfg,
    semantic: str,
) -> torch.Tensor:
    """First column of a body field (e.g. ground_clearance, contact_force)."""

    return _concat_fields(state, entity.body_field_names, semantic)[:, 0]


def body_field_mean(
    state: Mapping[str, torch.Tensor],
    entity: RobotEntityCfg,
    semantic: str,
) -> torch.Tensor:
    return _concat_fields(state, entity.body_field_names, semantic).mean(dim=1)


def terrain_height_extrema(
    state: Mapping[str, torch.Tensor],
    entity: RobotEntityCfg,
) -> tuple[torch.Tensor, torch.Tensor]:
    height = _concat_fields(state, entity.body_field_names, "terrain_height")
    return height.amax(dim=1), height.amin(dim=1)


def action_rate(ctx: StepContext) -> torch.Tensor:
    return (ctx.policy_actions - ctx.previous_policy_actions).pow(2).sum(dim=1)


def action_clip_fraction(policy_actions: torch.Tensor, *, clip: float) -> torch.Tensor:
    return (policy_actions.abs() > clip).float().mean(dim=1)


def _quat_rotate_inverse(quat_xyzw: torch.Tensor, vector: torch.Tensor) -> torch.Tensor:
    xyz = quat_xyzw[:, :3]
    w = quat_xyzw[:, 3:4]
    return cast(
        torch.Tensor,
        vector * (2.0 * w.square() - 1.0)
        - 2.0 * w * torch.linalg.cross(xyz, vector, dim=1)
        + 2.0 * xyz * (xyz * vector).sum(dim=1, keepdim=True),
    )


def _concat_fields(
    state: Mapping[str, torch.Tensor],
    prefixes: tuple[str, ...],
    semantic: str,
) -> torch.Tensor:
    if not prefixes:
        raise ConfigError(
            f"entity resolved no targets for semantic {semantic!r}",
            code="CONFIG_OUT_OF_RANGE",
            path="entity",
        )
    tensors = []
    for prefix in prefixes:
        name = f"{prefix}.{semantic}"
        if name not in state:
            raise ConfigError(
                f"missing state field {name!r}",
                code="CONFIG_MISSING_FIELD",
                path=name,
            )
        value = state[name]
        tensors.append(value.reshape(value.shape[0], -1))
    return torch.cat(tensors, dim=1)


__all__ = [
    "action_clip_fraction",
    "action_rate",
    "body_ang_vel_b",
    "body_field_mean",
    "body_lin_vel_b",
    "body_pose",
    "body_scalar_field",
    "planar_displacement",
    "terrain_height_extrema",
    "upright_from_pose",
]

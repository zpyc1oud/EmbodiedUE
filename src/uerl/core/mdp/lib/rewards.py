"""Task-agnostic reward term functions.

Unlike observation operators, reward terms are **not** constrained to the
cross-language operator set: they may use arbitrary PyTorch math. They never
enter the deploy/export path, so there is no parity corpus for these functions.
"""

from __future__ import annotations

from collections.abc import Mapping
from typing import cast

import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.direct.types import StepContext, TerminationResult
from uerl.errors import ConfigError


def track_lin_vel_xy(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
    std: float,
) -> torch.Tensor:
    """Exponential planar velocity tracking error in the body frame."""

    if std <= 0.0:
        raise ConfigError(
            f"track_lin_vel_xy std must be positive, got {std}",
            code="CONFIG_OUT_OF_RANGE",
            path="std",
        )
    pose = _body_pose(ctx.transition_state, entity)
    world_vel = _concat_fields(ctx.transition_state, entity.body_field_names, "body_linear_velocity")
    body_vel = _quat_rotate_inverse(pose[:, 3:7], world_vel[:, :3])
    command = _command_tensor(ctx, command_channel)
    error = (body_vel[:, :2] - command[:, :2]).pow(2).sum(dim=1)
    return torch.exp(-error / (std * std))


def track_ang_vel_z(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
    std: float,
) -> torch.Tensor:
    """Exponential yaw-rate tracking error in the body frame."""

    if std <= 0.0:
        raise ConfigError(
            f"track_ang_vel_z std must be positive, got {std}",
            code="CONFIG_OUT_OF_RANGE",
            path="std",
        )
    pose = _body_pose(ctx.transition_state, entity)
    world_vel = _concat_fields(ctx.transition_state, entity.body_field_names, "body_angular_velocity")
    body_vel = _quat_rotate_inverse(pose[:, 3:7], world_vel[:, :3])
    command = _command_tensor(ctx, command_channel)
    error = (body_vel[:, 2] - command[:, 2]).square()
    return torch.exp(-error / (std * std))


def joint_torque_l2(ctx: StepContext, *, entity: RobotEntityCfg) -> torch.Tensor:
    """Sum of squared applied joint torques for the selected joints."""

    torques = _concat_fields(ctx.transition_state, entity.joint_field_names, "applied_torque")
    return torques.pow(2).sum(dim=1)


def action_rate_l2(ctx: StepContext) -> torch.Tensor:
    """Sum of squared changes between consecutive policy actions (Isaac Lab ``action_rate_l2``).

    Exploration noise changes every decision independently of its interval, so
    the change is not divided by time; a ``rate`` reward term already scales it
    by the completed control interval.
    """

    return (ctx.policy_actions - ctx.previous_policy_actions).square().sum(dim=1)


def flat_orientation_l2(ctx: StepContext, *, entity: RobotEntityCfg) -> torch.Tensor:
    """L2 of the body-frame projected gravity xy components (flat = 0)."""

    pose = _body_pose(ctx.transition_state, entity)
    gravity_world = pose.new_tensor([0.0, 0.0, -1.0]).expand(pose.shape[0], 3)
    projected = _quat_rotate_inverse(pose[:, 3:7], gravity_world)
    return projected[:, :2].pow(2).sum(dim=1)


def is_alive(ctx: StepContext, *, terminations: TerminationResult) -> torch.Tensor:
    """1 where the row is not physically terminated (timeout still counts as alive)."""

    del ctx
    return (~terminations.terminated).to(dtype=torch.float32)


def is_terminated(ctx: StepContext, *, terminations: TerminationResult) -> torch.Tensor:
    """1 on physical termination rows (excludes pure timeout)."""

    del ctx
    return terminations.terminated.to(dtype=torch.float32)


def joint_pos_l2(ctx: StepContext, *, entity: RobotEntityCfg) -> torch.Tensor:
    """Sum of squared joint positions for the selected joints."""

    positions = _concat_fields(ctx.transition_state, entity.joint_field_names, "joint_position")
    return positions.pow(2).sum(dim=1)


def joint_vel_l1(ctx: StepContext, *, entity: RobotEntityCfg) -> torch.Tensor:
    """Sum of absolute joint velocities for the selected joints."""

    velocities = _concat_fields(ctx.transition_state, entity.joint_field_names, "joint_velocity")
    return velocities.abs().sum(dim=1)


def _quat_rotate_inverse(quat_xyzw: torch.Tensor, vector: torch.Tensor) -> torch.Tensor:
    """Rotate ``vector`` by the inverse of unit quaternion ``quat_xyzw`` (xyzw)."""

    xyz = quat_xyzw[:, :3]
    w = quat_xyzw[:, 3:4]
    return cast(
        torch.Tensor,
        vector * (2.0 * w.square() - 1.0)
        - 2.0 * w * torch.linalg.cross(xyz, vector, dim=1)
        + 2.0 * xyz * (xyz * vector).sum(dim=1, keepdim=True),
    )


def _command_tensor(ctx: StepContext, channel: str) -> torch.Tensor:
    if channel not in ctx.transition_state:
        raise ConfigError(
            f"missing command channel {channel!r} in transition_state",
            code="CONFIG_MISSING_FIELD",
            path=channel,
        )
    return ctx.transition_state[channel]


def _body_pose(state: Mapping[str, torch.Tensor], entity: RobotEntityCfg) -> torch.Tensor:
    return _concat_fields(state, entity.body_field_names, "body_pose")


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
    "action_rate_l2",
    "flat_orientation_l2",
    "is_alive",
    "is_terminated",
    "joint_pos_l2",
    "joint_torque_l2",
    "joint_vel_l1",
    "track_ang_vel_z",
    "track_lin_vel_xy",
]

"""Task-agnostic termination term functions."""

from __future__ import annotations

from collections.abc import Mapping

import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.direct.types import StepContext
from uerl.errors import ConfigError


def time_out(ctx: StepContext, *, max_steps: int) -> torch.Tensor:
    """Terminate when ``episode_steps >= max_steps`` (inclusive boundary)."""

    return ctx.episode_steps >= max_steps


def time_out_seconds(ctx: StepContext, *, max_seconds: float) -> torch.Tensor:
    """Truncate after a completed transition reaches the simulated-time horizon."""

    if ctx.episode_elapsed_s is None:
        raise ValueError("time_out_seconds requires episode_elapsed_s")
    if max_seconds <= 0.0:
        return torch.zeros_like(ctx.episode_steps, dtype=torch.bool)
    return ctx.episode_elapsed_s >= max_seconds


def bad_orientation(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    limit_rad: float,
) -> torch.Tensor:
    """Fail when projected tilt angle exceeds ``limit_rad`` (strict ``>``).

    Exactly ``limit_rad`` does **not** terminate.
    """

    pose = _body_pose(ctx.transition_state, entity)
    # upright = z-component of body z-axis ≈ 1 - 2(qx²+qy²) for XYZW quat.
    upright = 1.0 - 2.0 * (pose[:, 3].square() + pose[:, 4].square())
    tilt = torch.acos(upright.clamp(-1.0, 1.0))
    return tilt > limit_rad


def contact_force_exceeds(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    threshold_n: float,
) -> torch.Tensor:
    """Fail when any selected body's contact-force magnitude exceeds threshold (``>``)."""

    forces = _concat_fields(ctx.transition_state, entity.body_field_names, "contact_force")
    magnitude = torch.linalg.vector_norm(forces.reshape(forces.shape[0], -1, 3), dim=2).amax(dim=1)
    exceeded: torch.Tensor = magnitude > threshold_n
    return exceeded


def root_height_below(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    minimum_m: float,
) -> torch.Tensor:
    """Fail when the first selected body's height is strictly below ``minimum_m``."""

    pose = _body_pose(ctx.transition_state, entity)
    return pose[:, 2] < minimum_m


def joint_limit_violated(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
) -> torch.Tensor:
    """Fail when any selected joint position is outside its topology limits (``>`/`<``).

    Limits are bound onto ``entity`` during ``resolve`` so callers only pass the
    entity selector (ticket signature).
    """

    positions = _concat_fields(ctx.transition_state, entity.joint_field_names, "joint_position")
    violated = torch.zeros(positions.shape[0], dtype=torch.bool, device=positions.device)
    for column, (lower, upper) in enumerate(zip(entity.joint_lower_limits, entity.joint_upper_limits, strict=True)):
        value = positions[:, column]
        if lower is not None:
            violated |= value < lower
        if upper is not None:
            violated |= value > upper
    return violated


def joint_abs_exceeds(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    limit: float,
) -> torch.Tensor:
    """Fail when any selected joint's absolute position exceeds ``limit`` (strict ``>``)."""

    if limit <= 0.0:
        raise ConfigError(
            f"joint_abs_exceeds limit must be positive, got {limit}",
            code="CONFIG_OUT_OF_RANGE",
            path="limit",
        )
    positions = _concat_fields(ctx.transition_state, entity.joint_field_names, "joint_position")
    return positions.abs().amax(dim=1) > limit


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
    "bad_orientation",
    "contact_force_exceeds",
    "joint_abs_exceeds",
    "joint_limit_violated",
    "root_height_below",
    "time_out",
]

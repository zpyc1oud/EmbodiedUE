"""PhantomX-specific MDP term functions kept outside the shared operator set."""

from __future__ import annotations

from collections.abc import Mapping

import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec
from uerl.core.direct.robot_action import ROBOT_ACTUATOR_TARGET_FIELD
from uerl.core.direct.types import StepContext, TerminationResult
from uerl.core.mdp.lib import metrics as mdp_metrics
from uerl.errors import ConfigError
from uerl.tasks.phantomx.commands import velocity_command_from_latches
from uerl.tasks.phantomx.config import PHANTOMX_COMMAND_FIELDS, PHANTOMX_FEET, PhantomXTaskConfig

# Ticket 28 metric ownership. Keys stay ``phantomx/*`` (AC_003 / evaluator).
# ``lib`` — computed via ``core.mdp.lib.metrics``.
# ``reward_manager`` — mirrors a reward term; ``Episode_Reward/{term}`` is the
#   episode aggregate from RewardManager (per-step ``phantomx/*`` kept for eval).
# ``task`` — PhantomX-exclusive command / actuator / reset diagnostics.
PHANTOMX_METRIC_OWNERSHIP: Mapping[str, str] = {
    "phantomx/forward_velocity": "lib",
    "phantomx/lateral_velocity": "lib",
    "phantomx/yaw_rate": "lib",
    "phantomx/body_height": "lib",
    "phantomx/body_clearance": "lib",
    "phantomx/terrain_height_max": "lib",
    "phantomx/terrain_height_min": "lib",
    "phantomx/foot_contact_fraction": "lib",
    "phantomx/base_contact_force": "lib",
    "phantomx/base_contact": "lib",
    "phantomx/upright": "lib",
    "phantomx/displacement": "lib",
    "phantomx/action_clip_fraction": "lib",
    "phantomx/linear_velocity_progress": "reward_manager",
    "phantomx/yaw_rate_progress": "reward_manager",
    "phantomx/action_rate": "reward_manager",
    "phantomx/command_vx": "task",
    "phantomx/command_speed": "task",
    "phantomx/linear_velocity_error": "task",
    "phantomx/yaw_rate_error": "task",
    "phantomx/reset_age_steps": "task",
    "phantomx/reset_event": "task",
    "phantomx/reset_joint_error_max": "task",
    "phantomx/torque_clip_fraction": "task",
    "phantomx/torque_over_limit": "task",
}


def base_contact_force_exceeds(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    threshold_n: float,
) -> torch.Tensor:
    """Fail when scalar base contact force exceeds threshold (legacy PhantomX)."""

    forces = _concat_fields(ctx.transition_state, entity.body_field_names, "contact_force")
    return forces.reshape(forces.shape[0], -1)[:, 0] > threshold_n


def lin_vel_tracking(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
    std: float,
) -> torch.Tensor:
    velocity, command = _body_planar(ctx, entity=entity, command_channel=command_channel)
    return torch.exp(
        -(velocity[:, :2] - command[:, :2]).pow(2).sum(dim=1) / std**2
    )


def lin_vel_progress(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
    moving_threshold: float,
) -> torch.Tensor:
    velocity, command = _body_planar(ctx, entity=entity, command_channel=command_channel)
    command_speed = torch.linalg.vector_norm(command[:, :2], dim=1)
    moving = command_speed >= moving_threshold
    return torch.where(
        moving,
        torch.clamp(
            (velocity[:, :2] * command[:, :2]).sum(dim=1)
            / command_speed.pow(2).clamp_min(moving_threshold**2),
            -1.0,
            1.0,
        ),
        0.0,
    )


def yaw_rate_tracking(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
    std: float,
) -> torch.Tensor:
    _, angular_velocity, command = _body_rates(ctx, entity=entity, command_channel=command_channel)
    return torch.exp(-(angular_velocity[:, 2] - command[:, 2]).pow(2) / std**2)


def yaw_rate_progress(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
    moving_threshold: float,
) -> torch.Tensor:
    _, angular_velocity, command = _body_rates(ctx, entity=entity, command_channel=command_channel)
    turning = command[:, 2].abs() >= moving_threshold
    return torch.where(
        turning,
        torch.clamp(
            angular_velocity[:, 2]
            * command[:, 2]
            / command[:, 2].pow(2).clamp_min(moving_threshold**2),
            -1.0,
            1.0,
        ),
        0.0,
    )


def vertical_velocity_l2(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    del config
    velocity = mdp_metrics.body_lin_vel_b(ctx, entity=entity)
    return velocity[:, 2].pow(2)


def body_angular_xy_l2(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    angular_velocity = mdp_metrics.body_ang_vel_b(ctx, entity=entity)
    del config
    return angular_velocity[:, :2].pow(2).sum(dim=1)


def upright_penalty(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    del config
    pose = mdp_metrics.body_pose(ctx.transition_state, entity)
    upright = mdp_metrics.upright_from_pose(pose)
    return 1.0 - upright.pow(2)


def body_clearance_penalty(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    clearance = mdp_metrics.body_scalar_field(
        ctx.transition_state, entity, "ground_clearance"
    )
    return torch.relu(config.body_clearance_penalty_threshold - clearance).pow(2)


def joint_velocity_mean_l2(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    del config
    velocities = _concat_fields(ctx.transition_state, entity.joint_field_names, "joint_velocity")
    return velocities.pow(2).mean(dim=1)


def fall_contact(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    return base_contact_force_exceeds(
        ctx, entity=entity, threshold_n=config.base_contact_force_threshold
    ).float()


def collect_phantomx_metrics(
    ctx: StepContext,
    terminations: TerminationResult,
    *,
    config: PhantomXTaskConfig,
    robot_spec: RobotSpec,
) -> Mapping[str, torch.Tensor | float]:
    """Assemble the pre-migration ``phantomx/*`` key set (AC_003).

    Ownership is recorded in ``PHANTOMX_METRIC_OWNERSHIP``: lib helpers,
    reward-term mirrors, and task-exclusive diagnostics.
    """

    del terminations
    entity = RobotEntityCfg(body_names="base_link")
    entity.resolve(robot_spec)
    feet = RobotEntityCfg(body_names=tuple(PHANTOMX_FEET))
    feet.resolve(robot_spec)
    joints = RobotEntityCfg(joint_names=tuple(actuator.joint for actuator in robot_spec.actuators))
    joints.resolve(robot_spec)

    pose = mdp_metrics.body_pose(ctx.transition_state, entity)
    velocity = mdp_metrics.body_lin_vel_b(ctx, entity=entity)
    angular_velocity = mdp_metrics.body_ang_vel_b(ctx, entity=entity)
    command = _velocity_from_state(ctx.transition_state, pose, config)
    body_height = pose[:, 2]
    clearance = mdp_metrics.body_scalar_field(
        ctx.transition_state, entity, "ground_clearance"
    )
    terrain_max, terrain_min = mdp_metrics.terrain_height_extrema(
        ctx.transition_state, entity
    )
    base_contact_force = mdp_metrics.body_scalar_field(
        ctx.transition_state, entity, "contact_force"
    )
    base_contact = base_contact_force > config.base_contact_force_threshold
    foot_contact_fraction = mdp_metrics.body_field_mean(
        ctx.transition_state, feet, "contact"
    )
    upright = mdp_metrics.upright_from_pose(pose)
    command_speed = torch.linalg.vector_norm(command[:, :2], dim=1)
    displacement = mdp_metrics.planar_displacement(pose)
    defaults = ctx.policy_actions.new_tensor(
        [actuator.default_pos for actuator in robot_spec.actuators]
    )
    joint_position = _concat_fields(ctx.raw_state, joints.joint_field_names, "joint_position")
    joint_velocity = _concat_fields(ctx.raw_state, joints.joint_field_names, "joint_velocity")
    target = ctx.physical_command[ROBOT_ACTUATOR_TARGET_FIELD]
    stiffness = joint_position.new_tensor([actuator.stiffness for actuator in robot_spec.actuators])
    damping = joint_position.new_tensor([actuator.damping for actuator in robot_spec.actuators])
    effort_limit = joint_position.new_tensor(
        [actuator.effort_limit for actuator in robot_spec.actuators]
    )
    requested_effort = stiffness * (target - joint_position) - damping * joint_velocity
    torque_clip = requested_effort.abs() > effort_limit
    metrics = {
        "phantomx/forward_velocity": velocity[:, 0],
        "phantomx/lateral_velocity": velocity[:, 1],
        "phantomx/command_vx": command[:, 0],
        "phantomx/command_speed": command_speed,
        "phantomx/linear_velocity_error": torch.linalg.vector_norm(
            velocity[:, :2] - command[:, :2], dim=1
        ),
        "phantomx/linear_velocity_progress": lin_vel_progress(
            ctx,
            entity=entity,
            command_channel="velocity",
            moving_threshold=config.moving_command_threshold,
        ),
        "phantomx/yaw_rate": angular_velocity[:, 2],
        "phantomx/yaw_rate_error": (angular_velocity[:, 2] - command[:, 2]).abs(),
        "phantomx/yaw_rate_progress": yaw_rate_progress(
            ctx,
            entity=entity,
            command_channel="velocity",
            moving_threshold=config.moving_command_threshold,
        ),
        "phantomx/body_height": body_height,
        "phantomx/body_clearance": clearance,
        "phantomx/terrain_height_max": terrain_max,
        "phantomx/terrain_height_min": terrain_min,
        "phantomx/foot_contact_fraction": foot_contact_fraction,
        "phantomx/base_contact_force": base_contact_force,
        "phantomx/base_contact": base_contact.float(),
        "phantomx/upright": upright,
        "phantomx/displacement": displacement,
        "phantomx/reset_age_steps": ctx.episode_steps.to(dtype=torch.float32),
        "phantomx/reset_event": ctx.episode_steps.eq(1).float(),
        "phantomx/reset_joint_error_max": torch.where(
            ctx.episode_steps.eq(1),
            (joint_position - defaults).abs().amax(dim=1),
            torch.zeros_like(displacement),
        ),
        "phantomx/action_clip_fraction": mdp_metrics.action_clip_fraction(
            ctx.policy_actions, clip=config.action_clip
        ),
        "phantomx/torque_clip_fraction": torque_clip.float().mean(dim=1),
        "phantomx/torque_over_limit": torch.relu(requested_effort.abs() - effort_limit).mean(dim=1),
        "phantomx/action_rate": mdp_metrics.action_rate(ctx),
    }
    if set(metrics) != set(PHANTOMX_METRIC_OWNERSHIP):
        raise ConfigError(
            "phantomx metric keys drifted from PHANTOMX_METRIC_OWNERSHIP",
            code="CONFIG_OUT_OF_RANGE",
            path="PHANTOMX_METRIC_OWNERSHIP",
        )
    return metrics


def _body_planar(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
) -> tuple[torch.Tensor, torch.Tensor]:
    velocity = mdp_metrics.body_lin_vel_b(ctx, entity=entity)
    command = _published_or_latched_command(ctx, command_channel)
    return velocity, command


def _body_rates(
    ctx: StepContext,
    *,
    entity: RobotEntityCfg,
    command_channel: str,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    velocity = mdp_metrics.body_lin_vel_b(ctx, entity=entity)
    angular_velocity = mdp_metrics.body_ang_vel_b(ctx, entity=entity)
    command = _published_or_latched_command(ctx, command_channel)
    return velocity, angular_velocity, command


def _published_or_latched_command(ctx: StepContext, channel: str) -> torch.Tensor:
    if channel not in ctx.transition_state:
        raise ConfigError(
            f"missing published command channel {channel!r}",
            code="CONFIG_MISSING_FIELD",
            path=channel,
        )
    value = ctx.transition_state[channel]
    return value.reshape(value.shape[0], -1)


def _velocity_from_state(
    state: Mapping[str, torch.Tensor],
    pose: torch.Tensor,
    config: PhantomXTaskConfig,
) -> torch.Tensor:
    if "velocity" in state:
        return state["velocity"].reshape(pose.shape[0], 3)
    missing = [name for name in PHANTOMX_COMMAND_FIELDS if name not in state]
    if missing:
        # Prefer explicit velocity channel when curriculum fields are absent.
        if "velocity" in state:
            return state["velocity"].reshape(pose.shape[0], 3)
        raise ConfigError(
            f"missing PhantomX command fields {missing!r}",
            code="CONFIG_MISSING_FIELD",
            path="transition_state",
        )
    return velocity_command_from_latches(
        pose=pose,
        initial_linear=state[PHANTOMX_COMMAND_FIELDS[0]].reshape(pose.shape[0], 2),
        heading=state[PHANTOMX_COMMAND_FIELDS[1]].reshape(pose.shape[0], 1),
        post_turn_linear=state[PHANTOMX_COMMAND_FIELDS[2]].reshape(pose.shape[0], 2),
        config=config,
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
    "PHANTOMX_METRIC_OWNERSHIP",
    "base_contact_force_exceeds",
    "body_angular_xy_l2",
    "body_clearance_penalty",
    "collect_phantomx_metrics",
    "fall_contact",
    "joint_velocity_mean_l2",
    "lin_vel_progress",
    "lin_vel_tracking",
    "upright_penalty",
    "vertical_velocity_l2",
    "yaw_rate_progress",
    "yaw_rate_tracking",
]

"""Compose contact-based Catch from the shared PhantomX locomotion task."""

from __future__ import annotations

from dataclasses import dataclass, fields, replace

import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.models import DirectTaskConfig
from uerl.core.config.robot import RobotSpec
from uerl.core.direct.robot_observation import ObservationShapeTable
from uerl.core.direct.task import DirectTaskCfg
from uerl.core.direct.types import StepContext
from uerl.core.mdp.terms import DoneTermCfg, RewTermCfg
from uerl.tasks.phantomx.commands import PhantomXPursuitCommandSource
from uerl.tasks.phantomx.composed import build_phantomx_walk_cfg
from uerl.tasks.phantomx.config import PhantomXTaskConfig
from uerl.tasks.phantomx.mdp_terms import base_contact_force_exceeds
from uerl.tasks.phantomx.pursuit import PHANTOMX_PURSUIT_TARGET_DESCRIPTOR, PhantomXPursuitCommand
from uerl.tasks.phantomx.task import PhantomXTask

TARGET_CAPTURE_FIELD = "environment.target_capture"
TARGET_CAPTURE_DESCRIPTOR = {
    "name": TARGET_CAPTURE_FIELD,
    "dtype": "float32",
    "shape": (1,),
    "unit": "1",
    "frame": "none",
    "semantic": "target_capture",
    "source": "uerl.environment",
    "extensions": {},
}


@dataclass(frozen=True, slots=True)
class PhantomXCatchTaskConfig(PhantomXTaskConfig):
    """Add a once-per-capture reward to the locomotion objective."""

    capture_bonus: float = 1000.0


def capture_without_fall(
    ctx: StepContext, *, entity: RobotEntityCfg, threshold_n: float,
) -> torch.Tensor:
    """Accept the Environment contact latch only when the robot has not fallen."""

    contact = ctx.transition_state[TARGET_CAPTURE_FIELD].reshape(-1) > 0.5
    fallen = base_contact_force_exceeds(ctx, entity=entity, threshold_n=threshold_n)
    return contact & ~fallen


def capture_reward(ctx: StepContext, *, entity: RobotEntityCfg, threshold_n: float) -> torch.Tensor:
    return capture_without_fall(ctx, entity=entity, threshold_n=threshold_n).float()


def build_phantomx_catch_cfg(
    params: PhantomXCatchTaskConfig, robot_spec: RobotSpec, *, control_dt: float,
) -> DirectTaskCfg:
    base = build_phantomx_walk_cfg(params, robot_spec, control_dt=control_dt)
    arguments = {
        "entity": RobotEntityCfg(body_names="base_link"),
        "threshold_n": params.base_contact_force_threshold,
    }
    return replace(
        base,
        terminations=replace(base.terminations, terms={
            **base.terminations.terms,
            "capture": DoneTermCfg(func=capture_without_fall, params=arguments),
        }),
        rewards=replace(base.rewards, terms={
            **base.rewards.terms,
            # A capture is a discrete event, independent of control-window duration.
            "capture": RewTermCfg(func=capture_reward, weight=params.capture_bonus, params=arguments),
        }),
    )


def create_phantomx_catch_direct_task(
    config: DirectTaskConfig, *, robot_spec: RobotSpec | None = None,
    control_dt: float, batch_size: int, device: str | torch.device,
    observation_shapes: ObservationShapeTable | None = None,
) -> PhantomXTask:
    base = PhantomXTaskConfig.from_direct(config)
    params = base if isinstance(base, PhantomXCatchTaskConfig) else PhantomXCatchTaskConfig(
        **{item.name: getattr(base, item.name) for item in fields(base)}
    )
    source = PhantomXPursuitCommandSource(
        config=params, pursuit=PhantomXPursuitCommand(stopping_distance_m=0.0),
        batch_size=batch_size, device=device,
    )
    return PhantomXTask(
        params=params, control_dt=control_dt, batch_size=batch_size, device=device,
        robot_spec=robot_spec, command_source=source,
        cfg_builder=lambda task_params, spec: build_phantomx_catch_cfg(params, spec, control_dt=control_dt),
        extra_state=(PHANTOMX_PURSUIT_TARGET_DESCRIPTOR, TARGET_CAPTURE_DESCRIPTOR),
        observation_shapes=observation_shapes,
    )

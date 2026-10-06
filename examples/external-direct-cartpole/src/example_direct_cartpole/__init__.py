"""A minimal CartPole Task implemented with the public Python hooks."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import replace
from importlib.resources import files

import torch

from uerl.core.config.models import DirectTaskConfig
from uerl.core.config.robot import ObsType, RobotSpec
from uerl.core.config.yaml_loader import load_unique_yaml
from uerl.core.direct.robot_observation import ObservationShapeTable
from uerl.core.direct.task import DirectTask
from uerl.core.direct.types import PhysicalCommandBatch, StepContext, TerminationResult
from uerl.tasks.cartpole import CARTPOLE_CONTROL_DT, CartPoleTaskConfig
from uerl.tasks.cartpole.registration import create_cartpole_registration, create_cartpole_task_config
from uerl.tasks.registry import TaskRegistration

TASK_ID = "UERL-DirectCartPole-v0"
_POLE_POSITION = "robot.joint.pole.joint_position"
_POLE_VELOCITY = "robot.joint.pole.joint_velocity"
_CART_POSITION = "robot.joint.cart.joint_position"
_CART_VELOCITY = "robot.joint.cart.joint_velocity"
_OBSERVATION_FIELDS = (_POLE_POSITION, _POLE_VELOCITY, _CART_POSITION, _CART_VELOCITY)
_ACTION_FIELD = "robot.actuator.target"
_CARTPOLE_OBSERVATION_SHAPES = ObservationShapeTable(
    {ObsType.JOINT_POSITION: (1,), ObsType.JOINT_VELOCITY: (1,)}
)


class DirectCartPoleTask(DirectTask):
    """Implement actions, observations, rewards, and termination directly."""

    def __init__(self, config: CartPoleTaskConfig, *, robot_spec: RobotSpec | None = None) -> None:
        self.params = config
        direct_config = replace(
            config,
            state_requirements=_OBSERVATION_FIELDS,
            action_schema=(_ACTION_FIELD,),
        )
        super().__init__(
            direct_config,
            robot_spec=robot_spec,
            observation_shapes=(
                None if robot_spec is None else _CARTPOLE_OBSERVATION_SHAPES
            ),
            control_dt=CARTPOLE_CONTROL_DT,
            batch_size=1,
            device="cpu",
        )

    def preprocess_actions(
        self,
        policy_actions: torch.Tensor,
        raw_state: Mapping[str, torch.Tensor],
    ) -> PhysicalCommandBatch:
        del raw_state
        if self.robot_spec is None or len(self.robot_spec.actuators) != 1:
            raise RuntimeError("Direct CartPole requires one bound Robot actuator")
        scale = self.robot_spec.actuators[0].action_scale
        bounded = policy_actions.clamp(-self.params.action_clip, self.params.action_clip)
        return PhysicalCommandBatch({_ACTION_FIELD: bounded * scale})

    def build_observations(
        self,
        raw_state: Mapping[str, torch.Tensor],
        state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor,
        control_frame_dt: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        del previous_policy_actions, control_frame_dt
        observations = torch.cat([raw_state[name].reshape(-1, 1) for name in _OBSERVATION_FIELDS], dim=1)
        valid = state_valid.to(device=observations.device, dtype=torch.bool).unsqueeze(1)
        return {"policy": torch.where(valid, observations, torch.zeros_like(observations))}

    def compute_terminations(self, context: StepContext) -> TerminationResult:
        state = context.transition_state
        cart_out = state[_CART_POSITION].reshape(-1).abs() > self.params.max_cart_position
        pole_fell = state[_POLE_POSITION].reshape(-1).abs() > self.params.pole_angle_limit
        terminated = cart_out | pole_fell
        timed_out = (
            context.episode_steps >= self.params.max_episode_steps - 1
            if self.params.max_episode_steps > 0
            else torch.zeros_like(terminated)
        )
        return TerminationResult(
            terminated,
            timed_out,
            {"cart_out_of_bounds": cart_out, "pole_fell": pole_fell, "time_out": timed_out},
        )

    def compute_rewards(
        self,
        context: StepContext,
        terminations: TerminationResult,
    ) -> torch.Tensor:
        state = context.transition_state
        pole_position = state[_POLE_POSITION].reshape(-1)
        cart_velocity = state[_CART_VELOCITY].abs().reshape(-1)
        pole_velocity = state[_POLE_VELOCITY].abs().reshape(-1)
        ended = terminations.terminated.to(dtype=torch.float32)
        return (
            self.params.rew_scale_alive * (1.0 - ended)
            + self.params.rew_scale_terminated * ended
            + self.params.rew_scale_pole_pos * pole_position.square()
            + self.params.rew_scale_cart_vel * cart_velocity
            + self.params.rew_scale_pole_vel * pole_velocity
        )


def create_task_config() -> CartPoleTaskConfig:
    """Read this package's YAML reward override on each fresh Task config."""

    reward = load_unique_yaml(files(__package__).joinpath("reward.yaml").read_text(encoding="utf-8"))
    weight = reward.get("pole_position_weight") if isinstance(reward, dict) else None
    if not isinstance(weight, (int, float)) or isinstance(weight, bool):
        raise ValueError("reward.yaml must define a numeric pole_position_weight")
    return replace(
        create_cartpole_task_config(),
        rew_scale_pole_pos=float(weight),
        state_requirements=_OBSERVATION_FIELDS,
        action_schema=(_ACTION_FIELD,),
    )


def create_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
) -> DirectCartPoleTask:
    """Build fresh Python hooks around the resolved CartPole parameters."""

    return DirectCartPoleTask(CartPoleTaskConfig.from_direct(config), robot_spec=robot_spec)


def create_registration() -> TaskRegistration:
    """Reuse the built-in CartPole Worker, runner, and Session defaults."""

    return replace(
        create_cartpole_registration(),
        task_id=TASK_ID,
        task_version="0.1.0",
        task_factory=create_task,
        task_config_factory=create_task_config,
    )


__all__ = ["TASK_ID", "DirectCartPoleTask", "create_registration", "create_task", "create_task_config"]

"""Build CartPole ``DirectTaskCfg`` from typed task parameters + RobotSpec."""

from __future__ import annotations

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec
from uerl.core.direct.task import DirectTaskCfg
from uerl.core.mdp import lib as mdp
from uerl.core.mdp.terms import (
    ActionCfg,
    ActionTermCfg,
    DoneTermCfg,
    ObservationCfg,
    ObsGroupCfg,
    ObsTermCfg,
    RewardCfg,
    RewTermCfg,
    TerminationCfg,
)
from uerl.errors import ConfigError
from uerl.tasks.cartpole.config import CartPoleTaskConfig


def build_cartpole_composed_cfg(params: CartPoleTaskConfig, robot_spec: RobotSpec) -> DirectTaskCfg:
    """Expand CartPole YAML/task parameters into manager declarations.

    Reward scales are applied once per control transition. They do not depend on
    the number of physics frames used to complete that transition.
    """

    if len(robot_spec.actuators) != 1:
        raise ConfigError(
            "Cart-Pole requires exactly one Robot actuator",
            code="CONFIG_OUT_OF_RANGE",
            path="robot.actuators",
        )
    actuator = robot_spec.actuators[0]
    clip = (-params.action_clip, params.action_clip)

    observations = ObservationCfg(
        groups={
            "policy": ObsGroupCfg(
                terms={
                    "pole_pos": ObsTermCfg(
                        op="select",
                        params={
                            "entity": RobotEntityCfg(joint_names="pole"),
                            "field": "joint_position",
                        },
                    ),
                    "pole_vel": ObsTermCfg(
                        op="select",
                        params={
                            "entity": RobotEntityCfg(joint_names="pole"),
                            "field": "joint_velocity",
                        },
                    ),
                    "cart_pos": ObsTermCfg(
                        op="select",
                        params={
                            "entity": RobotEntityCfg(joint_names="cart"),
                            "field": "joint_position",
                        },
                    ),
                    "cart_vel": ObsTermCfg(
                        op="select",
                        params={
                            "entity": RobotEntityCfg(joint_names="cart"),
                            "field": "joint_velocity",
                        },
                    ),
                }
            )
        }
    )

    termination_terms: dict[str, DoneTermCfg] = {
        "cart_out_of_bounds": DoneTermCfg(
            func=mdp.terminations.joint_abs_exceeds,
            params={
                "entity": RobotEntityCfg(joint_names="cart"),
                "limit": params.max_cart_position,
            },
        ),
        "pole_fell": DoneTermCfg(
            func=mdp.terminations.joint_abs_exceeds,
            params={
                "entity": RobotEntityCfg(joint_names="pole"),
                "limit": params.pole_angle_limit,
            },
        ),
    }
    if params.max_episode_steps > 0:
        # Old CartPole timed out at ``episode_steps >= max_episode_steps - 1``.
        termination_terms["time_out"] = DoneTermCfg(
            func=mdp.terminations.time_out,
            time_out=True,
            params={"max_steps": params.max_episode_steps - 1},
        )

    return DirectTaskCfg(
        task=params,
        actions=ActionCfg(
            terms={
                "cart": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names=actuator.joint),
                    target_mode="effort",
                    clip=clip,
                    scale=actuator.action_scale,
                    use_default_offset=False,
                )
            }
        ),
        observations=observations,
        terminations=TerminationCfg(terms=termination_terms),
        rewards=RewardCfg(
            terms={
                "alive": RewTermCfg(
                    func=mdp.rewards.is_alive,
                    weight=params.rew_scale_alive,
                    consumes_terminations=True,
                ),
                "terminated": RewTermCfg(
                    func=mdp.rewards.is_terminated,
                    weight=params.rew_scale_terminated,
                    consumes_terminations=True,
                ),
                "pole_pos": RewTermCfg(
                    func=mdp.rewards.joint_pos_l2,
                    weight=params.rew_scale_pole_pos,
                    params={"entity": RobotEntityCfg(joint_names="pole")},
                ),
                "cart_vel": RewTermCfg(
                    func=mdp.rewards.joint_vel_l1,
                    weight=params.rew_scale_cart_vel,
                    params={"entity": RobotEntityCfg(joint_names="cart")},
                ),
                "pole_vel": RewTermCfg(
                    func=mdp.rewards.joint_vel_l1,
                    weight=params.rew_scale_pole_vel,
                    params={"entity": RobotEntityCfg(joint_names="pole")},
                ),
            }
        ),
    )


__all__ = ["build_cartpole_composed_cfg"]

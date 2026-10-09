"""Build PhantomX ``DirectTaskCfg`` variants via ``replace`` derivation."""

from __future__ import annotations

from dataclasses import replace

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec
from uerl.core.direct.task import DirectTaskCfg
from uerl.core.mdp import lib as mdp
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm
from uerl.core.mdp.terms import (
    ActionCfg,
    ActionTermCfg,
    CurriculumCfg,
    CurrTermCfg,
    DoneTermCfg,
    ObservationCfg,
    ObsGroupCfg,
    ObsTermCfg,
    RewardCfg,
    RewTermCfg,
    TerminationCfg,
)
from uerl.tasks.phantomx import mdp_terms as px_mdp
from uerl.tasks.phantomx.config import (
    PHANTOMX_CONTROL_FRAME_DT_SCALE,
    PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH,
    PHANTOMX_FEET,
    PHANTOMX_JOINTS,
    PHANTOMX_TERRAIN_CONFIG_PATH,
    PhantomXTaskConfig,
)
from uerl.tasks.phantomx.curriculum import PhantomXCommandCurriculum


def build_phantomx_observation_cfg(robot_spec: RobotSpec) -> ObservationCfg:
    """Declarative observation terms matching ticket 11 / ``build_phantomx_observation_plan``."""

    defaults_by_joint = {joint.name: joint.default_position for joint in robot_spec.topology.joints}
    defaults_by_joint.update({actuator.joint: actuator.default_pos for actuator in robot_spec.actuators})
    terms: dict[str, ObsTermCfg] = {
        "pose": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.body_pose"},
        ),
        "lin_vel_w": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.body_linear_velocity"},
        ),
        "ang_vel_w": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.body_angular_velocity"},
        ),
        "quat": ObsTermCfg(op="slice", inputs=("pose",), params={"start": 3, "width": 4}),
        "lin_vel_b": ObsTermCfg(op="rotate_inverse", inputs=("quat", "lin_vel_w")),
        "ang_vel_b": ObsTermCfg(op="rotate_inverse", inputs=("quat", "ang_vel_w")),
        "gravity_b": ObsTermCfg(
            op="projected_gravity",
            inputs=("quat",),
            params={"gravity": (0.0, 0.0, -1.0)},
        ),
        "velocity_cmd": ObsTermCfg(
            op="command",
            params={"channel": "velocity", "width": 3},
        ),
        "terrain": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.terrain_height"},
        ),
        "clearance": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.ground_clearance"},
        ),
    }
    contact_slots: list[str] = []
    for foot in PHANTOMX_FEET:
        slot = f"contact_{foot}"
        terms[slot] = ObsTermCfg(
            op="select",
            params={"field": f"robot.body.{foot}.contact"},
        )
        contact_slots.append(slot)
    terms["contacts"] = ObsTermCfg(op="concat", inputs=tuple(contact_slots))

    joint_pos_slots: list[str] = []
    joint_vel_slots: list[str] = []
    for joint in PHANTOMX_JOINTS:
        pos = f"jp_{joint}"
        vel = f"jv_{joint}"
        terms[pos] = ObsTermCfg(
            op="select",
            params={"field": f"robot.joint.{joint}.joint_position"},
        )
        terms[vel] = ObsTermCfg(
            op="select",
            params={"field": f"robot.joint.{joint}.joint_velocity"},
        )
        joint_pos_slots.append(pos)
        joint_vel_slots.append(vel)
    terms["joint_pos"] = ObsTermCfg(op="concat", inputs=tuple(joint_pos_slots))
    terms["joint_pos_rel"] = ObsTermCfg(
        op="joint_pos_rel",
        inputs=("joint_pos",),
        params={"default": tuple(defaults_by_joint[joint] for joint in PHANTOMX_JOINTS)},
    )
    terms["joint_vel"] = ObsTermCfg(op="concat", inputs=tuple(joint_vel_slots))
    terms["prev_action"] = ObsTermCfg(
        op="previous_action", params={"width": len(robot_spec.actuators)}
    )
    terms["control_frame_dt"] = ObsTermCfg(
        op="control_frame_dt", params={"scale": PHANTOMX_CONTROL_FRAME_DT_SCALE}
    )

    members = (
        "lin_vel_b",
        "ang_vel_b",
        "gravity_b",
        "velocity_cmd",
        "terrain",
        "clearance",
        "contacts",
        "joint_pos_rel",
        "joint_vel",
        "prev_action",
        "control_frame_dt",
    )
    return ObservationCfg(groups={"policy": ObsGroupCfg(terms=terms, members=members)})


def build_phantomx_action_cfg(robot_spec: RobotSpec, *, action_clip: float) -> ActionCfg:
    clip = (-action_clip, action_clip)
    terms: dict[str, ActionTermCfg] = {}
    for actuator in robot_spec.actuators:
        terms[actuator.joint] = ActionTermCfg(
            entity=RobotEntityCfg(joint_names=actuator.joint),
            target_mode="position",
            clip=clip,
            scale=actuator.action_scale,
            use_default_offset=True,
        )
    return ActionCfg(terms=terms)


def build_phantomx_walk_cfg(
    params: PhantomXTaskConfig,
    robot_spec: RobotSpec,
    *,
    control_dt: float,
) -> DirectTaskCfg:
    """Flat-walk composed task (pursuit reuses this MDP; terrain variants ``replace`` it)."""

    base = RobotEntityCfg(body_names="base_link")
    joints = RobotEntityCfg(joint_names=tuple(PHANTOMX_JOINTS))

    terminations = TerminationCfg(
        terms={
            "base_contact": DoneTermCfg(
                func=px_mdp.base_contact_force_exceeds,
                params={
                    "entity": base,
                    "threshold_n": params.base_contact_force_threshold,
                },
            ),
            "timeout": DoneTermCfg(
                func=mdp.terminations.time_out_seconds,
                time_out=True,
                params={"max_seconds": params.max_episode_duration_s or 0.0},
            ),
        }
    )

    rewards = RewardCfg(
        reference_dt_s=params.reference_dt_s,
        terms={
            "lin_vel_tracking": RewTermCfg(
                func=(
                    mdp.rewards.track_lin_vel_xy
                    if params.tracking_reward == "exponential"
                    else px_mdp.lin_vel_tracking
                ),
                weight=params.linear_velocity_tracking_weight,
                time_mode="rate",
                params={
                    "entity": base,
                    "command_channel": "velocity",
                    "std": params.velocity_tracking_std,
                },
            ),
            "lin_vel_progress": RewTermCfg(
                func=px_mdp.lin_vel_progress,
                weight=params.linear_velocity_progress_weight,
                time_mode="rate",
                params={
                    "entity": base,
                    "command_channel": "velocity",
                    "moving_threshold": params.moving_command_threshold,
                },
            ),
            "yaw_rate_tracking": RewTermCfg(
                func=(
                    mdp.rewards.track_ang_vel_z
                    if params.tracking_reward == "exponential"
                    else px_mdp.yaw_rate_tracking
                ),
                weight=params.yaw_rate_tracking_weight,
                time_mode="rate",
                params={
                    "entity": base,
                    "command_channel": "velocity",
                    "std": params.yaw_rate_tracking_std,
                },
            ),
            "yaw_rate_progress": RewTermCfg(
                func=px_mdp.yaw_rate_progress,
                weight=params.yaw_rate_progress_weight,
                time_mode="rate",
                params={
                    "entity": base,
                    "command_channel": "velocity",
                    "moving_threshold": params.moving_command_threshold,
                },
            ),
            "vertical_velocity": RewTermCfg(
                func=px_mdp.vertical_velocity_l2,
                weight=-params.vertical_velocity_weight,
                time_mode="rate",
                params={"entity": base, "config": params},
            ),
            "body_angular_velocity": RewTermCfg(
                func=px_mdp.body_angular_xy_l2,
                weight=-params.body_angular_velocity_weight,
                time_mode="rate",
                params={"entity": base, "config": params},
            ),
            "upright": RewTermCfg(
                func=px_mdp.upright_penalty,
                weight=-params.upright_weight,
                time_mode="rate",
                params={"entity": base, "config": params},
            ),
            "body_clearance": RewTermCfg(
                func=px_mdp.body_clearance_penalty,
                weight=-params.body_clearance_weight,
                time_mode="rate",
                params={"entity": base, "config": params},
            ),
            "action_rate": RewTermCfg(
                func=mdp.rewards.action_rate_l2,
                weight=-params.action_rate_weight,
                time_mode="rate",
                params={},
            ),
            "joint_velocity": RewTermCfg(
                func=px_mdp.joint_velocity_mean_l2,
                weight=-params.joint_velocity_penalty,
                time_mode="rate",
                params={"entity": joints, "config": params},
            ),
            "fall": RewTermCfg(
                func=px_mdp.fall_contact,
                weight=-params.fall_penalty,
                params={"entity": base, "config": params},
            ),
        }
    )

    return DirectTaskCfg(
        task=params,
        actions=build_phantomx_action_cfg(robot_spec, action_clip=params.action_clip),
        observations=build_phantomx_observation_cfg(robot_spec),
        terminations=terminations,
        rewards=rewards,
        curriculum=_phantomx_command_curriculum_cfg(),
        terrain_config_path=None,
    )


def _phantomx_command_curriculum_cfg() -> CurriculumCfg:
    """Declarative command curriculum (runtime params filled by curriculum factory)."""

    return CurriculumCfg(
        terms={
            "command": CurrTermCfg(term_class=PhantomXCommandCurriculum, params={}),
        }
    )


def _phantomx_terrain_curriculum_cfg() -> CurriculumCfg:
    """Command + terrain-level terms (terrain instance still assembled at train time)."""

    return CurriculumCfg(
        terms={
            "command": CurrTermCfg(term_class=PhantomXCommandCurriculum, params={}),
            "terrain": CurrTermCfg(term_class=TerrainLevelTerm, params={}),
        }
    )


def build_phantomx_continuous_terrain_cfg(
    params: PhantomXTaskConfig,
    robot_spec: RobotSpec,
    *,
    control_dt: float,
) -> DirectTaskCfg:
    """Continuous terrain: walk MDP + terrain curriculum + continuous atlas path."""

    return replace(
        build_phantomx_walk_cfg(params, robot_spec, control_dt=control_dt),
        curriculum=_phantomx_terrain_curriculum_cfg(),
        terrain_config_path=PHANTOMX_TERRAIN_CONFIG_PATH,
    )


def build_phantomx_discrete_terrain_cfg(
    params: PhantomXTaskConfig,
    robot_spec: RobotSpec,
    *,
    control_dt: float,
) -> DirectTaskCfg:
    """Discrete terrain: continuous cfg with discrete atlas path."""

    return replace(
        build_phantomx_continuous_terrain_cfg(params, robot_spec, control_dt=control_dt),
        terrain_config_path=PHANTOMX_DISCRETE_TERRAIN_CONFIG_PATH,
    )


__all__ = [
    "build_phantomx_action_cfg",
    "build_phantomx_continuous_terrain_cfg",
    "build_phantomx_discrete_terrain_cfg",
    "build_phantomx_observation_cfg",
    "build_phantomx_walk_cfg",
]

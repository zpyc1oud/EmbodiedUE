"""Built-in PhantomX Robot asset declaration."""

from __future__ import annotations

from ...core.config.robot import ObsType, ResetTargetType
from .base import ActuatorGroupCfg, ObsSelectorCfg, ResetTargetCfg, RobotAssetCfg, RobotInitStateCfg

PHANTOMX_ASSET_REF = "robots/phantomx/robot.yaml"
PHANTOMX_ASSET_PATH = "/Game/Robots/PhantomX/SK_PhantomX.SK_PhantomX"
PHANTOMX_JOINTS = (
    "c1_rf",
    "thigh_rf",
    "tibia_rf",
    "c1_rm",
    "thigh_rm",
    "tibia_rm",
    "c1_rr",
    "thigh_rr",
    "tibia_rr",
    "c1_lf",
    "thigh_lf",
    "tibia_lf",
    "c1_lm",
    "thigh_lm",
    "tibia_lm",
    "c1_lr",
    "thigh_lr",
    "tibia_lr",
)
PHANTOMX_FEET = (
    "tibia_rf",
    "tibia_rm",
    "tibia_rr",
    "tibia_lf",
    "tibia_lm",
    "tibia_lr",
)

PHANTOMX_CFG = RobotAssetCfg(
    name="phantomx",
    asset_path=PHANTOMX_ASSET_PATH,
    joint_names=PHANTOMX_JOINTS,
    body_names=("base_link", *PHANTOMX_JOINTS),
    init_state=RobotInitStateCfg(
        root_height_m=0.18,
        joint_pos={"c1_.*": 0.0, "thigh_.*": 0.15, "tibia_.*": -0.30},
    ),
    actuators=(
        ActuatorGroupCfg(
            joint_names=".*",
            target_mode="position",
            stiffness=25.0,
            damping=0.5,
            effort_limit=2.8,
            action_scale=0.20,
        ),
    ),
    observations=(
        ObsSelectorCfg(ObsType.JOINT_POSITION, joint_names=".*"),
        ObsSelectorCfg(ObsType.JOINT_VELOCITY, joint_names=".*"),
        ObsSelectorCfg(ObsType.BODY_POSE, body_names="base_link"),
        ObsSelectorCfg(ObsType.BODY_LINEAR_VELOCITY, body_names="base_link"),
        ObsSelectorCfg(ObsType.BODY_ANGULAR_VELOCITY, body_names="base_link"),
        ObsSelectorCfg(ObsType.GROUND_CLEARANCE, body_names="base_link"),
        ObsSelectorCfg(ObsType.TERRAIN_HEIGHT, body_names="base_link"),
        ObsSelectorCfg(ObsType.CONTACT, body_names=PHANTOMX_FEET, preserve_order=True),
        ObsSelectorCfg(
            ObsType.CONTACT_FORCE,
            body_names=("base_link", *PHANTOMX_FEET),
            preserve_order=True,
        ),
    ),
    reset=(
        ResetTargetCfg(
            target_type=ResetTargetType.ROOT_POSE,
            lower=(0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0),
            upper=(0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0),
            stream_id="reset.phantomx.root_pose",
        ),
        ResetTargetCfg(
            target_type=ResetTargetType.JOINT_POSITION,
            joint_names=".*",
            distribution_type="constant",
            lower=0.0,
            upper=0.0,
            stream_id="reset.phantomx.{joint}",
        ),
    ),
)


__all__ = [
    "PHANTOMX_ASSET_PATH",
    "PHANTOMX_ASSET_REF",
    "PHANTOMX_CFG",
    "PHANTOMX_FEET",
    "PHANTOMX_JOINTS",
]

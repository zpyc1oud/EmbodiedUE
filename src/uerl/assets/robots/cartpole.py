"""Built-in Cart-Pole Robot asset declaration."""

from __future__ import annotations

from ...core.config.robot import ObsType, ResetTargetType
from .base import ActuatorGroupCfg, ObsSelectorCfg, ResetTargetCfg, RobotAssetCfg, RobotInitStateCfg

CARTPOLE_ASSET_REF = "robots/cartpole/robot.yaml"
CARTPOLE_ASSET_PATH = "/Game/Robots/CartPole/SKM_CartPole"

CARTPOLE_CFG = RobotAssetCfg(
    name="cartpole",
    asset_path=CARTPOLE_ASSET_PATH,
    joint_names=("cart", "pole"),
    body_names=("base", "cart", "pole"),
    init_state=RobotInitStateCfg(joint_pos={"cart": 0.0, "pole": 0.0}),
    actuators=(
        ActuatorGroupCfg(
            joint_names="cart",
            target_mode="effort",
            stiffness=0.0,
            damping=1.0,
            effort_limit=100.0,
            action_scale=100.0,
        ),
    ),
    observations=(
        ObsSelectorCfg(ObsType.JOINT_POSITION, joint_names="pole"),
        ObsSelectorCfg(ObsType.JOINT_VELOCITY, joint_names="pole"),
        ObsSelectorCfg(ObsType.JOINT_POSITION, joint_names="cart"),
        ObsSelectorCfg(ObsType.JOINT_VELOCITY, joint_names="cart"),
    ),
    reset=(
        ResetTargetCfg(
            target_type=ResetTargetType.JOINT_POSITION,
            joint_names="cart",
            distribution_type="constant",
            lower=0.0,
            upper=0.0,
            stream_id="reset.cart.position",
        ),
        ResetTargetCfg(
            target_type=ResetTargetType.JOINT_VELOCITY,
            joint_names="cart",
            distribution_type="constant",
            lower=0.0,
            upper=0.0,
            stream_id="reset.cart.velocity",
        ),
        ResetTargetCfg(
            target_type=ResetTargetType.JOINT_POSITION,
            joint_names="pole",
            distribution_type="uniform",
            lower=-0.05,
            upper=0.05,
            stream_id="reset.pole.angle",
        ),
        ResetTargetCfg(
            target_type=ResetTargetType.JOINT_VELOCITY,
            joint_names="pole",
            distribution_type="constant",
            lower=0.0,
            upper=0.0,
            stream_id="reset.pole.velocity",
        ),
    ),
)


__all__ = ["CARTPOLE_ASSET_PATH", "CARTPOLE_ASSET_REF", "CARTPOLE_CFG"]

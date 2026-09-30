"""Verify the Python-owned built-in Robot asset declaration library."""

from __future__ import annotations

from dataclasses import replace

import pytest

from uerl import ConfigError
from uerl.assets.robots import (
    CARTPOLE_ASSET_REF,
    CARTPOLE_CFG,
    PHANTOMX_ASSET_REF,
    PHANTOMX_CFG,
    get_robot_asset,
    load_robot_asset,
    replace_actuators,
)
from uerl.assets.robots.base import RobotInitStateCfg
from uerl.assets.robots.phantomx import PHANTOMX_JOINTS
from uerl.core.config.robot import parse_robot_config

# Test-only migration snapshots. They preserve the exact old YAML contract
# after configs/robots is removed; runtime declarations live in assets/robots.
_LEGACY_CARTPOLE_YAML = """
actuators:
  - joint: cart
    stiffness: 0.0
    damping: 1.0
    effort_limit: 100.0
    default_pos: 0.0
    action_scale: 100.0
observations:
  - type: joint_position
    joint: pole
  - type: joint_velocity
    joint: pole
  - type: joint_position
    joint: cart
  - type: joint_velocity
    joint: cart
reset:
  distributions:
    - type: joint_position
      joint: cart
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.cart.position
    - type: joint_velocity
      joint: cart
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.cart.velocity
    - type: joint_position
      joint: pole
      distribution:
        type: uniform
        lower: -0.05
        upper: 0.05
        stream_id: reset.pole.angle
    - type: joint_velocity
      joint: pole
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.pole.velocity
"""

_LEGACY_PHANTOMX_YAML = """
actuators:
  - {joint: c1_rf, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.0, action_scale: 0.20}
  - {joint: thigh_rf, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.15, action_scale: 0.20}
  - {joint: tibia_rf, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: -0.30, action_scale: 0.20}
  - {joint: c1_rm, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.0, action_scale: 0.20}
  - {joint: thigh_rm, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.15, action_scale: 0.20}
  - {joint: tibia_rm, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: -0.30, action_scale: 0.20}
  - {joint: c1_rr, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.0, action_scale: 0.20}
  - {joint: thigh_rr, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.15, action_scale: 0.20}
  - {joint: tibia_rr, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: -0.30, action_scale: 0.20}
  - {joint: c1_lf, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.0, action_scale: 0.20}
  - {joint: thigh_lf, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.15, action_scale: 0.20}
  - {joint: tibia_lf, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: -0.30, action_scale: 0.20}
  - {joint: c1_lm, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.0, action_scale: 0.20}
  - {joint: thigh_lm, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.15, action_scale: 0.20}
  - {joint: tibia_lm, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: -0.30, action_scale: 0.20}
  - {joint: c1_lr, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.0, action_scale: 0.20}
  - {joint: thigh_lr, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: 0.15, action_scale: 0.20}
  - {joint: tibia_lr, stiffness: 25.0, damping: 0.5, effort_limit: 2.8, default_pos: -0.30, action_scale: 0.20}
observations:
  - {type: joint_position, joint: c1_rf}
  - {type: joint_position, joint: thigh_rf}
  - {type: joint_position, joint: tibia_rf}
  - {type: joint_position, joint: c1_rm}
  - {type: joint_position, joint: thigh_rm}
  - {type: joint_position, joint: tibia_rm}
  - {type: joint_position, joint: c1_rr}
  - {type: joint_position, joint: thigh_rr}
  - {type: joint_position, joint: tibia_rr}
  - {type: joint_position, joint: c1_lf}
  - {type: joint_position, joint: thigh_lf}
  - {type: joint_position, joint: tibia_lf}
  - {type: joint_position, joint: c1_lm}
  - {type: joint_position, joint: thigh_lm}
  - {type: joint_position, joint: tibia_lm}
  - {type: joint_position, joint: c1_lr}
  - {type: joint_position, joint: thigh_lr}
  - {type: joint_position, joint: tibia_lr}
  - {type: joint_velocity, joint: c1_rf}
  - {type: joint_velocity, joint: thigh_rf}
  - {type: joint_velocity, joint: tibia_rf}
  - {type: joint_velocity, joint: c1_rm}
  - {type: joint_velocity, joint: thigh_rm}
  - {type: joint_velocity, joint: tibia_rm}
  - {type: joint_velocity, joint: c1_rr}
  - {type: joint_velocity, joint: thigh_rr}
  - {type: joint_velocity, joint: tibia_rr}
  - {type: joint_velocity, joint: c1_lf}
  - {type: joint_velocity, joint: thigh_lf}
  - {type: joint_velocity, joint: tibia_lf}
  - {type: joint_velocity, joint: c1_lm}
  - {type: joint_velocity, joint: thigh_lm}
  - {type: joint_velocity, joint: tibia_lm}
  - {type: joint_velocity, joint: c1_lr}
  - {type: joint_velocity, joint: thigh_lr}
  - {type: joint_velocity, joint: tibia_lr}
  - {type: body_pose, body: base_link}
  - {type: body_linear_velocity, body: base_link}
  - {type: body_angular_velocity, body: base_link}
  - {type: ground_clearance, body: base_link}
  - {type: terrain_height, body: base_link}
  - {type: contact, body: tibia_rf}
  - {type: contact, body: tibia_rm}
  - {type: contact, body: tibia_rr}
  - {type: contact, body: tibia_lf}
  - {type: contact, body: tibia_lm}
  - {type: contact, body: tibia_lr}
  - {type: contact_force, body: base_link}
  - {type: contact_force, body: tibia_rf}
  - {type: contact_force, body: tibia_rm}
  - {type: contact_force, body: tibia_rr}
  - {type: contact_force, body: tibia_lf}
  - {type: contact_force, body: tibia_lm}
  - {type: contact_force, body: tibia_lr}
reset:
  distributions:
    - type: root_pose
      distribution:
        type: constant
        lower: [0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0]
        upper: [0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0]
        stream_id: reset.phantomx.root_pose
    - type: joint_position
      joint: c1_rf
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.c1_rf
    - type: joint_position
      joint: thigh_rf
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.thigh_rf
    - type: joint_position
      joint: tibia_rf
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.tibia_rf
    - type: joint_position
      joint: c1_rm
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.c1_rm
    - type: joint_position
      joint: thigh_rm
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.thigh_rm
    - type: joint_position
      joint: tibia_rm
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.tibia_rm
    - type: joint_position
      joint: c1_rr
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.c1_rr
    - type: joint_position
      joint: thigh_rr
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.thigh_rr
    - type: joint_position
      joint: tibia_rr
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.tibia_rr
    - type: joint_position
      joint: c1_lf
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.c1_lf
    - type: joint_position
      joint: thigh_lf
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.thigh_lf
    - type: joint_position
      joint: tibia_lf
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.tibia_lf
    - type: joint_position
      joint: c1_lm
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.c1_lm
    - type: joint_position
      joint: thigh_lm
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.thigh_lm
    - type: joint_position
      joint: tibia_lm
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.tibia_lm
    - type: joint_position
      joint: c1_lr
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.c1_lr
    - type: joint_position
      joint: thigh_lr
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.thigh_lr
    - type: joint_position
      joint: tibia_lr
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.phantomx.tibia_lr
"""


@pytest.mark.parametrize(
    ("reference", "snapshot"),
    [
        (CARTPOLE_ASSET_REF, _LEGACY_CARTPOLE_YAML),
        (PHANTOMX_ASSET_REF, _LEGACY_PHANTOMX_YAML),
    ],
)
def test_builtin_assets_match_the_migrated_robot_yaml_contract(reference: str, snapshot: str) -> None:
    """Compare every actuator, observation, and reset field with the old parser output."""

    assert load_robot_asset(reference) == parse_robot_config(snapshot, source="migration snapshot")


def test_phantomx_regex_defaults_expand_every_joint_and_later_patterns_win() -> None:
    """Use full-match selectors with explicit later-pattern-wins semantics."""

    expanded = PHANTOMX_CFG.init_state.expand_joint_positions(PHANTOMX_JOINTS)
    assert tuple(expanded) == PHANTOMX_JOINTS
    assert all(expanded[name] == 0.0 for name in PHANTOMX_JOINTS if name.startswith("c1_"))
    assert all(expanded[name] == 0.15 for name in PHANTOMX_JOINTS if name.startswith("thigh_"))
    assert all(expanded[name] == -0.30 for name in PHANTOMX_JOINTS if name.startswith("tibia_"))

    overlapping = replace(
        PHANTOMX_CFG,
        init_state=RobotInitStateCfg(joint_pos={"c1_.*": 0.0, ".*": 1.25}),
    )
    assert all(value == 1.25 for value in overlapping.init_state.expand_joint_positions(PHANTOMX_JOINTS).values())


def test_robot_asset_regex_zero_match_fails_with_available_names() -> None:
    """Do not silently accept a declaration pattern that selects nothing."""

    invalid = replace(
        PHANTOMX_CFG,
        init_state=RobotInitStateCfg(joint_pos={"c1_.*": 0.0, "missing_.*": 1.0}),
    )

    with pytest.raises(ConfigError) as raised:
        invalid.to_robot_config()

    assert raised.value.code == "CONFIG_OUT_OF_RANGE"
    assert "missing_.*" in str(raised.value)
    assert "c1_rf" in str(raised.value)


def test_replace_actuators_derives_without_mutating_the_builtin_asset() -> None:
    """Override group fields in a new declaration and preserve the source."""

    derived = replace_actuators(PHANTOMX_CFG, stiffness=40.0)

    assert derived is not PHANTOMX_CFG
    assert derived.actuators[0].stiffness == 40.0
    assert PHANTOMX_CFG.actuators[0].stiffness == 25.0
    assert derived.init_state == PHANTOMX_CFG.init_state
    assert derived.observations == PHANTOMX_CFG.observations
    assert derived.reset == PHANTOMX_CFG.reset
    assert derived.to_robot_config().actuators[0].stiffness == 40.0
    assert PHANTOMX_CFG.to_robot_config().actuators[0].stiffness == 25.0


def test_robot_asset_registry_resolves_only_registered_declaration_keys() -> None:
    """The stable key is a registry identity, never a filesystem lookup."""

    assert get_robot_asset(CARTPOLE_ASSET_REF) is CARTPOLE_CFG

    with pytest.raises(ConfigError) as raised:
        load_robot_asset("C:/outside/robot.yaml")

    assert raised.value.code == "ROBOT_ASSET_NOT_FOUND"
    assert raised.value.path == "robot.config_path"

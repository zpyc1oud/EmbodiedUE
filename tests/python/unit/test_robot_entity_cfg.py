"""Verify RobotEntityCfg name/regex binding against real RobotSpec fixtures."""

from __future__ import annotations

import pytest

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import (
    RobotSpec,
    RobotTopology,
    merge_robot_spec,
    parse_robot_config,
)
from uerl.errors import ConfigError
from uerl.tasks.phantomx.config import PHANTOMX_JOINTS, load_phantomx_training_config


def _small_robot_spec() -> RobotSpec:
    """Three joints / four bodies — real RobotSpec, not a mock."""

    config = parse_robot_config(
        """
actuators:
  - joint: j_a
    stiffness: 1.0
    damping: 0.1
    effort_limit: 1.0
    default_pos: 0.0
    action_scale: 1.0
observations:
  - type: joint_position
    joint: j_a
reset:
  distributions:
    - type: joint_position
      joint: j_a
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.j_a
"""
    )
    joint_frame = {
        "position_metres": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    topology = RobotTopology.from_response(
        {
            "asset_path": "/Game/Test/SK_Test",
            "body_names": ["base", "link_a", "link_thigh_l", "link_thigh_r"],
            "body_motion_types": ["kinematic", "simulated", "simulated", "simulated"],
            "root_body_index": 0,
            "fixed_base": True,
            "joints": [
                {
                    "name": "j_a",
                    "parent_body_index": 0,
                    "child_body_index": 1,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": joint_frame,
                    "parent_frame": joint_frame,
                },
                {
                    # Names chosen so topology order (z then a) ≠ alphabetical order.
                    "name": "z_thigh",
                    "parent_body_index": 0,
                    "child_body_index": 2,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": joint_frame,
                    "parent_frame": joint_frame,
                },
                {
                    "name": "a_thigh",
                    "parent_body_index": 0,
                    "child_body_index": 3,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": joint_frame,
                    "parent_frame": joint_frame,
                },
            ],
        }
    )
    return merge_robot_spec(config, topology)


def _phantomx_robot_spec() -> RobotSpec:
    training = load_phantomx_training_config()
    body_names = ["base_link", *PHANTOMX_JOINTS]
    joints: list[dict[str, object]] = []
    for index, joint in enumerate(PHANTOMX_JOINTS):
        parent_body_index = 0 if index % 3 == 0 else index
        joints.append(
            {
                "name": joint,
                "parent_body_index": parent_body_index,
                "child_body_index": index + 1,
                "degrees_of_freedom": 1,
                "coordinate": "twist",
                "coordinate_type": "revolute",
                "unit": "rad",
                "default_position": 0.0,
                "lower_limit": -3.141592653589793,
                "upper_limit": 3.141592653589793,
                "child_frame": {
                    "position_metres": [0.0, 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                "parent_frame": {
                    "position_metres": [float(index), 0.0, 0.0],
                    "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            }
        )
    topology = RobotTopology.from_response(
        {
            "asset_path": training.robot_asset_path,
            "body_names": body_names,
            "body_motion_types": ["simulated"] * len(body_names),
            "root_body_index": 0,
            "fixed_base": False,
            "joints": joints,
        }
    )
    return merge_robot_spec(training.robot_config, topology)


def test_ac_py_unit_entity_001_single_regex_topology_order() -> None:
    """AC_PY_UNIT_ENTITY_001: single regex hits follow topology order."""

    spec = _small_robot_spec()
    entity = RobotEntityCfg(joint_names=".*_thigh")
    entity.resolve(spec)

    assert entity.joint_ids == (1, 2)
    assert entity.joint_field_names == (
        "robot.joint.z_thigh",
        "robot.joint.a_thigh",
    )
    # Topology order must win over alphabetical (a_thigh < z_thigh).
    assert entity.joint_field_names != tuple(sorted(entity.joint_field_names))


def test_ac_py_unit_entity_002_preserve_order_swaps_with_list() -> None:
    """AC_PY_UNIT_ENTITY_002: list order changes result order when preserve_order."""

    spec = _small_robot_spec()
    forward = RobotEntityCfg(joint_names=("a_thigh", "z_thigh"), preserve_order=True)
    forward.resolve(spec)
    reverse = RobotEntityCfg(joint_names=("z_thigh", "a_thigh"), preserve_order=True)
    reverse.resolve(spec)

    assert forward.joint_ids == (2, 1)
    assert reverse.joint_ids == (1, 2)
    assert forward.joint_ids != reverse.joint_ids


def test_ac_py_unit_entity_003_zero_hit_lists_available_names() -> None:
    """AC_PY_UNIT_ENTITY_003: zero hit fails in resolve with available names."""

    spec = _small_robot_spec()
    entity = RobotEntityCfg(joint_names="no_such_.*")
    with pytest.raises(ConfigError) as raised:
        entity.resolve(spec)
    assert raised.value.path == "joint_names"
    assert "j_a" in str(raised.value)
    assert "z_thigh" in str(raised.value)


def test_ac_py_unit_entity_004_joint_and_body_resolve_independently() -> None:
    """AC_PY_UNIT_ENTITY_004: joint_names and body_names do not cross-contaminate."""

    spec = _small_robot_spec()
    entity = RobotEntityCfg(joint_names=".*_thigh", body_names="link_.*")
    entity.resolve(spec)

    assert entity.joint_ids == (1, 2)
    assert entity.body_ids == (1, 2, 3)
    assert entity.joint_field_names == (
        "robot.joint.z_thigh",
        "robot.joint.a_thigh",
    )
    assert entity.body_field_names == (
        "robot.body.link_a",
        "robot.body.link_thigh_l",
        "robot.body.link_thigh_r",
    )

def test_ac_py_unit_entity_005_resolve_idempotent_and_unread_before() -> None:
    """AC_PY_UNIT_ENTITY_005: unread before resolve errors; resolve is idempotent."""

    spec = _small_robot_spec()
    entity = RobotEntityCfg(joint_names="j_a")
    with pytest.raises(ConfigError) as unread:
        _ = entity.joint_ids
    assert unread.value.path == "resolved"

    entity.resolve(spec)
    first = entity.joint_ids
    entity.resolve(spec)
    assert entity.joint_ids == first == (0,)


def test_ac_py_unit_entity_006_phantomx_regex_matches_explicit_names() -> None:
    """AC_PY_UNIT_ENTITY_006: regex and explicit PhantomX thigh names yield same fields."""

    spec = _phantomx_robot_spec()
    thigh_names = tuple(name for name in PHANTOMX_JOINTS if name.startswith("thigh_"))
    assert len(thigh_names) == 6

    by_regex = RobotEntityCfg(joint_names="thigh_.*")
    by_regex.resolve(spec)
    by_names = RobotEntityCfg(joint_names=thigh_names)
    by_names.resolve(spec)

    assert by_regex.joint_field_names == by_names.joint_field_names
    assert by_regex.joint_ids == by_names.joint_ids
    assert by_regex.joint_field_names == tuple(f"robot.joint.{name}" for name in thigh_names)

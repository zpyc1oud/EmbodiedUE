"""Verify explicit RobotConfig parsing and RobotSpec binding."""

from __future__ import annotations

from pathlib import Path

import pytest

from uerl import (
    ActuatorConfig,
    ConfigError,
    ConstraintFrame,
    JointTopology,
    ObservationConfig,
    ObsType,
    ResetTarget,
    ResetTargetType,
    RobotConfig,
    RobotTopology,
    load_robot_config,
    merge_robot_spec,
    parse_robot_config,
)

_FRAME = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))


_VALID_YAML = """
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
    joint: cart
  - type: body_pose
    body: pole
reset:
  distributions:
    - type: joint_position
      joint: pole
      distribution:
        type: uniform
        lower: -0.05
        upper: 0.05
        stream_id: reset.pole
"""


def _joint(
    name: str,
    parent: int,
    child: int,
    coordinate: str,
    coordinate_type: str,
    unit: str,
    lower: float | None,
    upper: float | None,
    dof: int = 1,
    default_position: float = 0.0,
) -> JointTopology:
    return JointTopology(
        name, parent, child, dof, coordinate, coordinate_type, unit, lower, upper, _FRAME, _FRAME, default_position
    )


def _cartpole_topology(
    *,
    cart_limit: float | None = None,
    fixed_base: bool = True,
    pole_default: float = 0.0,
) -> RobotTopology:
    return RobotTopology(
        body_names=("base", "cart", "pole"),
        body_motion_types=(
            ("kinematic", "simulated", "simulated")
            if fixed_base
            else ("simulated", "simulated", "simulated")
        ),
        root_body_index=0,
        fixed_base=fixed_base,
        joints=(
            _joint(
                "cart",
                0,
                1,
                "linear_x",
                "prismatic",
                "m",
                -4.0 if cart_limit is not None else None,
                cart_limit,
                1,
            ),
            _joint("pole", 1, 2, "twist", "revolute", "rad", -3.14, 3.14, default_position=pole_default),
        ),
    )


def _topology_payload() -> dict[str, object]:
    return {
        "body_names": ["base", "cart", "pole"],
        "body_motion_types": ["kinematic", "simulated", "simulated"],
        "root_body_index": 0,
        "fixed_base": True,
        "joints": [
            {
                "name": "cart",
                "parent_body_index": 0,
                "child_body_index": 1,
                "degrees_of_freedom": 1,
                "coordinate": "linear_x",
                "coordinate_type": "prismatic",
                "unit": "m",
                "default_position": 0.0,
                "lower_limit": None,
                "upper_limit": None,
                "child_frame": {"position_metres": [0.0, 0.0, 0.0], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
                "parent_frame": {"position_metres": [0.0, 0.0, 0.0], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
            {
                "name": "pole",
                "parent_body_index": 1,
                "child_body_index": 2,
                "degrees_of_freedom": 1,
                "coordinate": "twist",
                "coordinate_type": "revolute",
                "unit": "rad",
                "default_position": 0.0,
                "lower_limit": -3.14,
                "upper_limit": 3.14,
                "child_frame": {"position_metres": [0.0, 0.0, 0.1], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
                "parent_frame": {"position_metres": [0.0, 0.0, 0.1], "rotation_xyzw": [0.0, 0.0, 0.0, 1.0]},
            },
        ],
    }


def test_parse_valid_config_produces_explicit_typed_declarations() -> None:
    config = parse_robot_config(_VALID_YAML)

    assert isinstance(config, RobotConfig)
    assert config.actuators == (ActuatorConfig("cart", 0.0, 1.0, 100.0, 0.0, 100.0),)
    assert config.observations == (
        ObservationConfig(ObsType.JOINT_POSITION, "pole", "joint"),
        ObservationConfig(ObsType.JOINT_VELOCITY, "cart", "joint"),
        ObservationConfig(ObsType.BODY_POSE, "pole", "body"),
    )
    assert config.reset.distributions == (
        ResetTarget(
            ResetTargetType.JOINT_POSITION,
            "pole",
            "uniform",
            -0.05,
            0.05,
            "reset.pole",
        ),
    )
    assert config.reset.distributions[0].wire_key == "joint_position:pole"


def test_contact_force_observation_is_parsed_as_a_body_observation() -> None:
    document = _VALID_YAML.replace(
        "  - type: body_pose\n    body: pole\n",
        "  - type: body_pose\n    body: pole\n  - type: contact_force\n    body: pole\n",
    )

    config = parse_robot_config(document)

    assert config.observations[-1] == ObservationConfig(ObsType.CONTACT_FORCE, "pole", "body")


def test_load_reads_asset_adjacent_file(tmp_path: Path) -> None:
    (tmp_path / "robot.yaml").write_text(_VALID_YAML, encoding="utf-8")
    assert load_robot_config(tmp_path).actuators[0].joint == "cart"


def test_missing_file_fails_loudly(tmp_path: Path) -> None:
    with pytest.raises(ConfigError) as raised:
        load_robot_config(tmp_path)
    assert raised.value.code == "ROBOT_CONFIG_NOT_FOUND"
    assert raised.value.path == str(tmp_path / "robot.yaml")


def test_invalid_yaml_and_duplicate_keys_fail_loudly() -> None:
    with pytest.raises(ConfigError, match="YAML"):
        parse_robot_config("actuators: [unterminated")
    with pytest.raises(ConfigError):
        parse_robot_config("actuators: []\nactuators: []\nobservations: []\nreset: {}")


def test_effort_limit_is_required() -> None:
    document = _VALID_YAML.replace("    effort_limit: 100.0\n", "")
    with pytest.raises(ConfigError, match="effort_limit"):
        parse_robot_config(document)


def test_numeric_fields_require_finite_numbers() -> None:
    with pytest.raises(ConfigError) as raised:
        parse_robot_config(_VALID_YAML.replace("    action_scale: 100.0\n", "    action_scale: .inf\n"))
    assert raised.value.code == "INVALID_ROBOT_CONFIG"
    assert raised.value.path == "actuators[0].action_scale"

    with pytest.raises(ConfigError) as raised:
        parse_robot_config(_VALID_YAML.replace("        lower: -0.05\n", "        lower: .nan\n"))
    assert raised.value.code == "INVALID_ROBOT_CONFIG"
    assert raised.value.path == "reset.distributions[0].distribution.lower"


def test_zero_gain_actuator_is_rejected() -> None:
    document = _VALID_YAML.replace("    damping: 1.0\n", "    damping: 0.0\n")
    with pytest.raises(ConfigError, match="zero-gain"):
        parse_robot_config(document)


def test_duplicate_actuator_is_rejected() -> None:
    duplicate = """  - joint: cart
    stiffness: 0.0
    damping: 1.0
    effort_limit: 100.0
    default_pos: 0.0
    action_scale: 100.0
"""
    document = _VALID_YAML.replace("    action_scale: 100.0\n", "    action_scale: 100.0\n" + duplicate, 1)

    with pytest.raises(ConfigError) as raised:
        parse_robot_config(document)

    assert raised.value.code == "DUPLICATE_ACTUATOR"
    assert raised.value.path == "actuators[1].joint"


def test_observation_targets_are_explicit_and_duplicates_keep_order() -> None:
    document = _VALID_YAML.replace(
        "  - type: body_pose\n    body: pole\n",
        "  - type: joint_position\n    joint: pole\n  - type: body_pose\n    body: pole\n",
    )
    config = parse_robot_config(document)
    assert [item.target for item in config.observations] == ["pole", "cart", "pole", "pole"]

    with pytest.raises(ConfigError, match="requires joint"):
        parse_robot_config(_VALID_YAML.replace("    joint: pole\n", "    body: pole\n", 1))
    with pytest.raises(ConfigError, match="requires body"):
        parse_robot_config(_VALID_YAML.replace("    body: pole\n", "    joint: pole\n", 1))


def test_reset_sampling_is_reproducible_and_slot_scoped() -> None:
    config = parse_robot_config(_VALID_YAML)
    first = config.reset.sample(seed=7, episode_index=3, slot_ids=(1, 4))
    second = config.reset.sample(seed=7, episode_index=3, slot_ids=(1, 4))
    changed_episode = config.reset.sample(seed=7, episode_index=4, slot_ids=(1, 4))

    assert first == second
    assert first != changed_episode
    assert len(first["joint_position:pole"]) == 2
    assert all(
        isinstance(value, float) and -0.05 <= value <= 0.05
        for value in first["joint_position:pole"]
    )


def test_ac_py_unit_generic_008_joint_reset_offsets_use_actuator_reference() -> None:
    reset_block = """    - type: joint_position
      joint: pole
      distribution:
        type: uniform
        lower: -0.05
        upper: 0.05
        stream_id: reset.pole
"""
    referenced_block = """    - type: joint_position
      joint: cart
      distribution:
        type: constant
        lower: 0.1
        upper: 0.1
        stream_id: reset.cart
"""
    document = _VALID_YAML.replace("default_pos: 0.0", "default_pos: 0.75").replace(
        reset_block, referenced_block
    )
    config = parse_robot_config(document)
    spec = merge_robot_spec(config, _cartpole_topology(cart_limit=1.0))

    sampled = config.reset.sample(seed=7, episode_index=1, slot_ids=(0,))

    assert spec.reset[0].reference == 0.75
    assert spec.resolve_reset_values(sampled, 0) == (0.85,)

    outside_limit = document.replace("lower: 0.1\n        upper: 0.1", "lower: 0.0\n        upper: 0.3")
    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(outside_limit), _cartpole_topology(cart_limit=1.0))
    assert raised.value.code == "RESET_POSITION_OUT_OF_RANGE"


def test_ac_py_unit_generic_009_passive_joint_reset_uses_asset_reference() -> None:
    config = parse_robot_config(_VALID_YAML)
    spec = merge_robot_spec(config, _cartpole_topology(pole_default=0.4))
    sampled = config.reset.sample(seed=7, episode_index=1, slot_ids=(0,))

    raw_offset = sampled["joint_position:pole"][0]

    assert isinstance(raw_offset, float)
    assert spec.reset[0].reference == 0.4
    assert spec.resolve_reset_values(sampled, 0) == pytest.approx((0.4 + raw_offset,))


def test_topology_from_response_builds_single_root_and_preserves_free_limits() -> None:
    topology = RobotTopology.from_response(_topology_payload())
    assert topology.body_names == ("base", "cart", "pole")
    assert topology.fixed_base is True
    assert topology.joints[0].lower_limit is None
    assert topology.joints[0].upper_limit is None
    assert topology.joints[0].default_position == 0.0
    assert topology.joints[1].coordinate_type == "revolute"


def test_topology_rejects_non_normalized_constraint_frame() -> None:
    payload = _topology_payload()
    joints = payload["joints"]
    assert isinstance(joints, list)
    first = dict(joints[0])
    first["child_frame"] = {"position_metres": [0.0, 0.0, 0.0], "rotation_xyzw": [0.0, 0.0, 0.0, 0.0]}
    payload["joints"] = [first, joints[1]]
    with pytest.raises(ValueError, match="constraint frame"):
        RobotTopology.from_response(payload)


def test_floating_root_reset_expands_absolute_pose_and_velocity_vectors() -> None:
    root_reset = _VALID_YAML.replace(
        """    - type: joint_position
      joint: pole
      distribution:
        type: uniform
        lower: -0.05
        upper: 0.05
        stream_id: reset.pole
""",
        """    - type: root_pose
      distribution:
        type: constant
        lower: [1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 1.0]
        upper: [1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 1.0]
        stream_id: reset.root.pose
    - type: root_velocity
      distribution:
        type: constant
        lower: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]
        upper: [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]
        stream_id: reset.root.velocity
""",
    )

    config = parse_robot_config(root_reset)
    sampled = config.reset.sample(seed=7, episode_index=1, slot_ids=(0,))
    spec = merge_robot_spec(config, _cartpole_topology(fixed_base=False))

    assert sampled["root_pose"] == ((1.0, 2.0, 3.0, 0.0, 0.0, 0.0, 1.0),)
    assert sampled["root_velocity"] == ((0.1, 0.2, 0.3, 0.4, 0.5, 0.6),)
    assert len(spec.reset) == 13
    assert [target.wire_key for target in spec.reset[:7]] == [f"root_pose:{index}" for index in range(7)]
    assert [target.wire_key for target in spec.reset[7:]] == [f"root_velocity:{index}" for index in range(6)]
    assert all(target.component_count == 7 for target in spec.reset[:7])
    assert all(target.component_count == 6 for target in spec.reset[7:])


def test_ac_py_unit_generic_004_merge_rejects_duplicate_observation_primitive() -> None:
    duplicate = """  - type: joint_position
    joint: pole
"""
    document = _VALID_YAML.replace("  - type: joint_velocity\n    joint: cart\n", duplicate)
    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(document), _cartpole_topology())

    assert raised.value.code == "DUPLICATE_OBSERVATION"
    assert raised.value.path == "observations[1]"

"""Verify binding of explicit Robot semantics to reflected topology."""

from __future__ import annotations

import pytest

from uerl import ConfigError, ConstraintFrame, JointTopology, RobotTopology, merge_robot_spec, parse_robot_config

_FRAME = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))


_ROBOT_YAML = """
actuators:
  - joint: cart
    stiffness: 10.0
    damping: 1.0
    effort_limit: 100.0
    default_pos: 0.0
    action_scale: 100.0
observations:
  - type: joint_position
    joint: cart
  - type: joint_velocity
    joint: pole
reset:
  distributions: []
"""


def _joint(
    name: str,
    parent: int,
    child: int,
    dof: int,
    coordinate: str,
    coordinate_type: str,
    unit: str,
    lower: float | None,
    upper: float | None,
) -> JointTopology:
    return JointTopology(name, parent, child, dof, coordinate, coordinate_type, unit, lower, upper, _FRAME, _FRAME, 0.0)


def _topology(*, cart_dof: int = 1, cart_upper: float = 1.0, duplicate_child: bool = False) -> RobotTopology:
    pole_parent = 0 if duplicate_child else 1
    return RobotTopology(
        body_names=("base", "cart", "pole"),
        body_motion_types=("kinematic", "simulated", "simulated"),
        root_body_index=0,
        fixed_base=True,
        joints=(
            _joint("cart", 0, 1, cart_dof, "linear_x", "prismatic", "m", -1.0, cart_upper),
            _joint("pole", pole_parent, 2, 1, "twist", "revolute", "rad", -1.57, 1.57),
        ),
    )


def test_merge_resolves_explicit_semantics_in_declared_order() -> None:
    spec = merge_robot_spec(parse_robot_config(_ROBOT_YAML), _topology())

    assert [item.index for item in spec.actuators] == [0]
    assert [item.joint_index for item in spec.actuators] == [0]
    assert [item.index for item in spec.observations] == [0, 1]
    assert [item.joint_index for item in spec.observations] == [0, 1]
    assert [item.body_index for item in spec.observations] == [1, 2]
    assert [item.index for item in spec.reset] == []
    assert spec.actuators[0].target_unit == "m"


def test_merge_preserves_declaration_order_for_same_topology() -> None:
    reordered_yaml = _ROBOT_YAML.replace(
        "  - type: joint_position\n    joint: cart\n  - type: joint_velocity\n    joint: pole\n",
        "  - type: joint_velocity\n    joint: pole\n  - type: joint_position\n    joint: cart\n",
    )

    original = merge_robot_spec(parse_robot_config(_ROBOT_YAML), _topology())
    reordered = merge_robot_spec(parse_robot_config(reordered_yaml), _topology())
    repeated = merge_robot_spec(parse_robot_config(_ROBOT_YAML), _topology())

    assert [(item.index, item.target) for item in original.observations] == [(0, "cart"), (1, "pole")]
    assert [(item.index, item.target) for item in reordered.observations] == [(0, "pole"), (1, "cart")]
    assert original == repeated
    assert original.observations != reordered.observations


def test_merge_rejects_unknown_actuator_joint() -> None:
    config = parse_robot_config(_ROBOT_YAML.replace("joint: cart", "joint: missing", 1))

    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(config, _topology())

    assert raised.value.code == "UNKNOWN_JOINT"
    assert raised.value.path == "actuators[0].joint"


def test_merge_rejects_default_position_outside_asset_limits() -> None:
    config = parse_robot_config(_ROBOT_YAML.replace("default_pos: 0.0", "default_pos: 2.0"))

    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(config, _topology(cart_upper=1.0))

    assert raised.value.code == "DEFAULT_POSITION_OUT_OF_RANGE"


def test_merge_rejects_duplicate_reset_target() -> None:
    document = _ROBOT_YAML.replace(
        "  distributions: []",
        """  distributions:
    - type: joint_position
      joint: cart
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.cart.position
    - type: joint_position
      joint: cart
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.cart.position.duplicate
""",
    )

    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(document), _topology())

    assert raised.value.code == "DUPLICATE_RESET_TARGET"
    assert raised.value.path == "reset.distributions[1]"


def test_merge_rejects_unknown_reset_joint() -> None:
    document = _ROBOT_YAML.replace(
        "  distributions: []",
        """  distributions:
    - type: joint_position
      joint: missing
      distribution:
        type: uniform
        lower: -0.5
        upper: 0.5
        stream_id: reset.missing
""",
    )

    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(document), _topology())

    assert raised.value.code == "UNKNOWN_JOINT"
    assert raised.value.path == "reset.distributions[0].joint"


def test_merge_accepts_joint_reset_inside_asset_limits() -> None:
    config = parse_robot_config(
        _ROBOT_YAML.replace(
            "  distributions: []",
            """  distributions:
    - type: joint_position
      joint: cart
      distribution:
        type: uniform
        lower: -0.5
        upper: 0.5
        stream_id: reset.cart
""",
        )
    )

    spec = merge_robot_spec(config, _topology(cart_upper=1.0))

    assert spec.reset[0].index == 0
    assert spec.reset[0].lower == -0.5
    assert spec.reset[0].upper == 0.5


def test_merge_rejects_joint_reset_outside_asset_limits() -> None:
    document = _ROBOT_YAML.replace(
        "  distributions: []",
        """  distributions:
    - type: joint_position
      joint: cart
      distribution:
        type: uniform
        lower: -2.0
        upper: 2.0
        stream_id: reset.cart
""",
    )

    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(document), _topology(cart_upper=1.0))

    assert raised.value.code == "RESET_POSITION_OUT_OF_RANGE"
    assert raised.value.path == "reset.distributions[0].distribution"


def test_merge_rejects_multi_dof_actuator() -> None:
    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(_ROBOT_YAML), _topology(cart_dof=2))

    assert raised.value.code == "ACTUATOR_DIMENSION_MISMATCH"
    assert raised.value.path == "actuators[0].joint"


def test_merge_rejects_unknown_explicit_observation_joint() -> None:
    config = parse_robot_config(
        _ROBOT_YAML.replace("  - type: joint_position\n    joint: cart", "  - type: joint_position\n    joint: missing")
    )

    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(config, _topology())

    assert raised.value.code == "UNKNOWN_JOINT"
    assert raised.value.path == "observations[0].joint"


def test_merge_rejects_unknown_explicit_observation_body() -> None:
    document = _ROBOT_YAML.replace(
        "  - type: joint_velocity\n    joint: pole",
        "  - type: body_pose\n    body: missing",
    )
    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(document), _topology())

    assert raised.value.code == "UNKNOWN_BODY"
    assert raised.value.path == "observations[1].target"


def test_merge_rejects_terrain_height_observation_on_non_root_body() -> None:
    document = _ROBOT_YAML.replace(
        "  - type: joint_position\n    joint: cart",
        "  - type: terrain_height\n    body: cart",
    )

    with pytest.raises(ConfigError) as raised:
        merge_robot_spec(parse_robot_config(document), _topology())

    assert raised.value.code == "OBSERVATION_TARGET_MISMATCH"
    assert raised.value.path == "observations[0].target"

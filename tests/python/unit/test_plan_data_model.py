"""Verify observation/action plan data model and JSON codec."""

from __future__ import annotations

from typing import Any

import pytest

from uerl.core.mdp.plan import PLAN_VERSION, ActionPlan, ObservationPlan, PlanOp
from uerl.errors import ConfigError


def _valid_observation_payload() -> dict[str, Any]:
    return {
        "plan_version": PLAN_VERSION,
        "state_requirements": [
            "robot.body.base_link.body_pose",
            "robot.joint.left_c1.joint_position",
        ],
        "ops": [
            {
                "op": "select",
                "inputs": [],
                "output": "pose",
                "width": 7,
                "params": {
                    "field": "robot.body.base_link.body_pose",
                    "enabled": True,
                    "scale": 1.5,
                    "bias": [0.0, 0.1, -0.2],
                    "axes": [0, 1, 2],
                },
            },
            {
                "op": "select",
                "inputs": [],
                "output": "joint",
                "width": 1,
                "params": {"field": "robot.joint.left_c1.joint_position"},
            },
            {
                "op": "concat",
                "inputs": ["pose", "joint"],
                "output": "policy_vec",
                "width": 8,
                "params": {},
            },
        ],
        "groups": {"policy": ["pose", "joint"], "debug": ["policy_vec"]},
        "group_widths": {"policy": 8, "debug": 8},
    }


def test_ac_py_unit_plan_001_to_json_from_json_round_trip_preserves_op_sequence() -> None:
    """AC_PY_UNIT_PLAN_001: round-trip is identical, including ops sequence."""

    # Arrange
    original = ObservationPlan.from_json(_valid_observation_payload())

    # Act
    restored = ObservationPlan.from_json(original.to_json())

    # Assert
    assert restored == original
    assert [op.op for op in restored.ops] == ["select", "select", "concat"]
    assert [op.output for op in restored.ops] == ["pose", "joint", "policy_vec"]
    assert list(restored.groups["policy"]) == ["pose", "joint"]
    assert restored.ops[0].params["enabled"] is True
    assert restored.ops[0].params["bias"] == (0.0, 0.1, -0.2)
    assert restored.ops[0].params["axes"] == (0, 1, 2)


def test_ac_py_unit_plan_002_validate_detects_cycle() -> None:
    """AC_PY_UNIT_PLAN_002: cyclic op dependencies raise PLAN_CYCLE at construction."""

    # Arrange / Act / Assert
    with pytest.raises(ConfigError) as exc_info:
        ObservationPlan(
            state_requirements=(),
            ops=(
                PlanOp("scale", ("b",), "a", 1),
                PlanOp("scale", ("a",), "b", 1),
            ),
            groups={},
            group_widths={},
        )
    assert exc_info.value.code == "PLAN_CYCLE"
    assert exc_info.value.path == "ops"


def test_ac_py_unit_plan_003_validate_detects_non_topological_order() -> None:
    """AC_PY_UNIT_PLAN_003: forward references raise PLAN_NOT_TOPOLOGICAL."""

    # Arrange / Act / Assert
    with pytest.raises(ConfigError) as exc_info:
        ObservationPlan(
            state_requirements=(),
            ops=(
                PlanOp("scale", ("later",), "early", 1),
                PlanOp("select", (), "later", 1, {"field": "robot.body.base_link.body_pose"}),
            ),
            groups={},
            group_widths={},
        )
    assert exc_info.value.code == "PLAN_NOT_TOPOLOGICAL"
    assert exc_info.value.path == "ops[0].inputs[0]"


def test_ac_py_unit_plan_004_validate_detects_duplicate_unknown_and_group_width() -> None:
    """AC_PY_UNIT_PLAN_004: duplicate slots, dangling refs, and group width mismatches."""

    # Arrange / Act / Assert — duplicate output
    with pytest.raises(ConfigError) as duplicate:
        ObservationPlan(
            state_requirements=(),
            ops=(
                PlanOp("select", (), "pose", 7, {"field": "robot.body.base_link.body_pose"}),
                PlanOp("select", (), "pose", 7, {"field": "robot.body.base_link.body_pose"}),
            ),
            groups={},
            group_widths={},
        )
    assert duplicate.value.code == "PLAN_DUPLICATE_SLOT"

    # Arrange / Act / Assert — unknown input slot
    with pytest.raises(ConfigError) as unknown:
        ObservationPlan(
            state_requirements=(),
            ops=(PlanOp("scale", ("missing",), "out", 1),),
            groups={},
            group_widths={},
        )
    assert unknown.value.code == "PLAN_UNKNOWN_SLOT"

    # Arrange / Act / Assert — group width mismatch
    with pytest.raises(ConfigError) as width:
        ObservationPlan(
            state_requirements=("robot.body.base_link.body_pose",),
            ops=(PlanOp("select", (), "pose", 7, {"field": "robot.body.base_link.body_pose"}),),
            groups={"policy": ("pose",)},
            group_widths={"policy": 3},
        )
    assert width.value.code == "PLAN_GROUP_MISMATCH"


def test_ac_py_unit_plan_005_plan_version_mismatch_rejects_without_parsing() -> None:
    """AC_PY_UNIT_PLAN_005: unsupported plan_version raises before accepting ops."""

    # Arrange
    payload = _valid_observation_payload()
    payload["plan_version"] = 99

    # Act / Assert
    with pytest.raises(ConfigError) as exc_info:
        ObservationPlan.from_json(payload)
    assert exc_info.value.code == "PLAN_VERSION_MISMATCH"
    assert exc_info.value.path == "plan_version"


def test_action_plan_round_trip_and_validation() -> None:
    """ActionPlan serializes and rejects unknown command fields at construction."""

    # Arrange
    plan = ActionPlan(
        ops=(
            PlanOp("policy_action", (), "raw", 2, {"width": 2}),
            PlanOp("clip", ("raw",), "target", 2, {"low": -1.0, "high": 1.0}),
        ),
        command_fields=("target",),
        policy_width=2,
    )

    # Act
    restored = ActionPlan.from_json(plan.to_json())

    # Assert
    assert restored == plan
    with pytest.raises(ConfigError) as exc_info:
        ActionPlan(
            ops=(PlanOp("policy_action", (), "raw", 2),),
            command_fields=("missing",),
            policy_width=2,
        )
    assert exc_info.value.code == "PLAN_UNKNOWN_SLOT"

"""Unit tests for the closed plan operator registry."""

from __future__ import annotations

import pytest
import torch

from uerl.core.mdp.executor import PlanExecutor
from uerl.core.mdp.operators import (
    OperatorSpec,
    register_operator,
    registered_operators,
    require_operator,
)
from uerl.core.mdp.plan import ObservationPlan, PlanOp
from uerl.errors import ConfigError


def test_ac_py_unit_ops_001_duplicate_register_and_unregistered_at_construction() -> None:
    """AC_PY_UNIT_OPS_001: duplicate register raises; unknown op fails at construct."""

    # Arrange / Act / Assert — duplicate name
    with pytest.raises(ConfigError) as duplicate:
        register_operator(
            OperatorSpec(
                name="select",
                arity=0,
                output_width=lambda _w, _p: 0,
                evaluate=lambda _a, _p: torch.zeros(1, 1),
                param_schema={"field": str},
            )
        )
    assert duplicate.value.code == "OP_DUPLICATE"

    # Arrange — plan with an unregistered operator name (structure still valid)
    plan = ObservationPlan(
        state_requirements=(),
        ops=(PlanOp("not_a_real_op", (), "out", 1),),
        groups={"policy": ("out",)},
        group_widths={"policy": 1},
    )

    # Act / Assert — fails at construction, not execute
    with pytest.raises(ConfigError) as unknown:
        PlanExecutor(plan)
    assert unknown.value.code == "OP_UNKNOWN"
    message = str(unknown.value)
    for name in registered_operators():
        assert name in message
    assert "select" in message
    assert "concat" in message


def test_source_vs_transform_split_mini_examples() -> None:
    """Source ops specialize in the executor; transform ops use evaluate."""

    select = require_operator("select")
    command = require_operator("command")
    previous_action = require_operator("previous_action")
    policy_action = require_operator("policy_action")
    concat = require_operator("concat")
    assert select.is_source is True
    assert command.is_source is True
    assert previous_action.is_source is True
    assert policy_action.is_source is True
    assert concat.is_source is False

    # Transform: unified evaluate
    a = torch.tensor([[1.0, 2.0]])
    b = torch.tensor([[3.0]])
    assert callable(concat.evaluate)
    evaluate = concat.evaluate
    assert callable(evaluate)
    out = evaluate((a, b), {})
    assert torch.equal(out, torch.tensor([[1.0, 2.0, 3.0]]))

    # Source: sentinel is not a callable tensor function
    assert select.evaluate is not concat.evaluate
    assert not callable(select.evaluate)


def test_missing_required_params_raise_at_construction() -> None:
    """Missing required select param raises at PlanExecutor construction."""

    plan = ObservationPlan(
        state_requirements=("robot.body.base_link.body_pose",),
        ops=(PlanOp("select", (), "pose", 7, {}),),
        groups={"policy": ("pose",)},
        group_widths={"policy": 7},
    )
    with pytest.raises(ConfigError) as exc_info:
        PlanExecutor(plan)
    assert exc_info.value.code == "OP_PARAM"
    assert "field" in str(exc_info.value)


def test_arity_mismatch_raises_at_construction() -> None:
    """Arity mismatches fail in PlanExecutor.__init__, not on first execute."""

    # select arity is 0; a state field as input is a valid plan DAG edge but wrong arity.
    field = "robot.body.base_link.body_pose"
    plan = ObservationPlan(
        state_requirements=(field,),
        ops=(PlanOp("select", (field,), "pose", 7, {"field": field}),),
        groups={"policy": ("pose",)},
        group_widths={"policy": 7},
    )
    with pytest.raises(ConfigError) as exc_info:
        PlanExecutor(plan)
    assert exc_info.value.code == "OP_ARITY"

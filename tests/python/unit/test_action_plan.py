"""Unit tests for ActionPlanExecutor and shared-operator reuse."""

from __future__ import annotations

import torch

from uerl.core.mdp.executor import ActionPlanExecutor, PlanExecutor
from uerl.core.mdp.operators import require_operator
from uerl.core.mdp.plan import ActionPlan, ObservationPlan, PlanOp
from uerl.errors import ConfigError


def _full_chain_action_plan(width: int = 4) -> ActionPlan:
    bias = tuple(float(i) * 0.05 for i in range(width))
    return ActionPlan(
        ops=(
            PlanOp("policy_action", (), "raw", width, {"width": width}),
            PlanOp("clip", ("raw",), "clipped", width, {"low": -1.0, "high": 1.0}),
            PlanOp("scale", ("clipped",), "scaled", width, {"factor": 0.2}),
            PlanOp("offset", ("scaled",), "target", width, {"bias": bias}),
        ),
        command_fields=("target",),
        policy_width=width,
    )


def test_action_plan_reuses_same_operator_specs_as_observation() -> None:
    """clip / scale / offset in action and observation plans share OperatorSpec identity."""

    obs = ObservationPlan(
        state_requirements=("robot.x",),
        ops=(
            PlanOp("select", (), "selected", 2, {"field": "robot.x"}),
            PlanOp("clip", ("selected",), "clipped", 2, {"low": -1.0, "high": 1.0}),
            PlanOp("scale", ("clipped",), "scaled", 2, {"factor": 0.5}),
            PlanOp("offset", ("scaled",), "out", 2, {"bias": 0.1}),
        ),
        groups={"policy": ("out",)},
        group_widths={"policy": 2},
    )
    act = _full_chain_action_plan(2)
    obs_executor = PlanExecutor(obs)
    act_executor = ActionPlanExecutor(act)

    obs_specs = {op.op: spec for op, spec in zip(obs.ops, obs_executor._specs, strict=True)}
    act_specs = {op.op: spec for op, spec in zip(act.ops, act_executor._specs, strict=True)}
    for name in ("clip", "scale", "offset"):
        assert obs_specs[name] is act_specs[name]
        assert obs_specs[name] is require_operator(name)


def test_action_plan_matches_legacy_decode_at_1e_6() -> None:
    """DefaultPosition + ActionScale * clamp(action, -1, 1) within 1e-6."""

    actions = torch.tensor([[0.3, -0.5, 0.7, -0.2, 1.5, -2.0],
                            [-1.5, 0.5, -0.7, 0.2, -0.3, 2.0]], dtype=torch.float32)
    defaults = (0.0, 0.15, -0.3, 0.05, 0.1, -0.2)
    scale = 0.2
    plan = ActionPlan(
        ops=(
            PlanOp("policy_action", (), "raw", 6, {"width": 6}),
            PlanOp("clip", ("raw",), "clipped", 6, {"low": -1.0, "high": 1.0}),
            PlanOp("scale", ("clipped",), "scaled", 6, {"factor": scale}),
            PlanOp("offset", ("scaled",), "target", 6, {"bias": defaults}),
        ),
        command_fields=("target",),
        policy_width=6,
    )
    got = ActionPlanExecutor(plan).execute(actions)["target"]
    legacy = torch.tensor([[0.06, 0.05, -0.16, 0.01, 0.30, -0.40],
                           [-0.20, 0.25, -0.44, 0.09, 0.04, 0.0]], dtype=torch.float32)
    assert torch.max(torch.abs(got - legacy)).item() <= 1.0e-6


def test_action_plan_command_field_width_mismatch_at_compile() -> None:
    """AC_PARITY_ACTION_003: produced width vs command_channels fails at Compile."""

    plan = _full_chain_action_plan(4)
    try:
        ActionPlanExecutor(plan, command_channels={"target": 2})
        raise AssertionError("expected ConfigError")
    except ConfigError as exc:
        assert exc.code == "OP_WIDTH"
        assert "target" in str(exc)


def test_policy_action_is_source_operator() -> None:
    """policy_action is an arity-0 source like previous_action."""

    spec = require_operator("policy_action")
    assert spec.is_source is True
    assert spec.arity == 0

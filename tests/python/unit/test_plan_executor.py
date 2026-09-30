"""Unit tests for PlanExecutor observation assembly."""

from __future__ import annotations

import pytest
import torch

from uerl.core.mdp.executor import PlanExecutor, PlanInputs
from uerl.core.mdp.plan import ObservationPlan, PlanOp
from uerl.errors import ConfigError

POSE_FIELD = "robot.body.base_link.body_pose"
JOINT_FIELD = "robot.joint.left_c1.joint_position"


def _select_concat_plan() -> ObservationPlan:
    return ObservationPlan(
        state_requirements=(POSE_FIELD, JOINT_FIELD),
        ops=(
            PlanOp("select", (), "pose", 7, {"field": POSE_FIELD}),
            PlanOp("select", (), "joint", 1, {"field": JOINT_FIELD}),
            PlanOp("concat", ("pose", "joint"), "policy_vec", 8),
        ),
        groups={"policy": ("pose", "joint"), "debug": ("policy_vec",)},
        group_widths={"policy": 8, "debug": 8},
    )


def test_ac_py_unit_ops_002_select_by_field_missing_raises() -> None:
    """AC_PY_UNIT_OPS_002: select by field name; missing field raises (no zero-fill)."""

    # Arrange
    plan = ObservationPlan(
        state_requirements=(POSE_FIELD,),
        ops=(PlanOp("select", (), "pose", 7, {"field": POSE_FIELD}),),
        groups={"policy": ("pose",)},
        group_widths={"policy": 7},
    )
    executor = PlanExecutor(plan)
    pose = torch.arange(7, dtype=torch.float32).unsqueeze(0)

    # Act
    out = executor.execute(PlanInputs(raw_state={POSE_FIELD: pose}))

    # Assert — selected by field name
    assert torch.equal(out["policy"], pose)

    # Act / Assert — missing field raises, does not zero-fill
    with pytest.raises(ConfigError) as exc_info:
        executor.execute(PlanInputs(raw_state={}))
    assert exc_info.value.code == "OP_MISSING_FIELD"
    assert POSE_FIELD in str(exc_info.value)


def test_ac_py_unit_ops_003_concat_order_with_distinguishable_widths() -> None:
    """AC_PY_UNIT_OPS_003: concat preserves input order (not only total width)."""

    # Arrange — three distinguishable widths with unique marker values
    plan = ObservationPlan(
        state_requirements=("a", "b", "c"),
        ops=(
            PlanOp("select", (), "s_a", 2, {"field": "a"}),
            PlanOp("select", (), "s_b", 3, {"field": "b"}),
            PlanOp("select", (), "s_c", 1, {"field": "c"}),
            PlanOp("concat", ("s_a", "s_b", "s_c"), "out", 6),
        ),
        groups={"policy": ("out",)},
        group_widths={"policy": 6},
    )
    executor = PlanExecutor(plan)
    # Markers: a=[10,11], b=[20,21,22], c=[30] — order errors cannot hide behind sum
    raw = {
        "a": torch.tensor([[10.0, 11.0]]),
        "b": torch.tensor([[20.0, 21.0, 22.0]]),
        "c": torch.tensor([[30.0]]),
    }

    # Act
    out = executor.execute(PlanInputs(raw_state=raw))

    # Assert
    assert torch.equal(out["policy"], torch.tensor([[10.0, 11.0, 20.0, 21.0, 22.0, 30.0]]))


def test_ac_py_unit_ops_004_declared_vs_actual_width_mismatch_raises() -> None:
    """AC_PY_UNIT_OPS_004: declared width vs actual output mismatch raises."""

    # Arrange / Act / Assert — construct-time: concat derived width != declared
    with pytest.raises(ConfigError) as construct:
        PlanExecutor(
            ObservationPlan(
                state_requirements=("a", "b"),
                ops=(
                    PlanOp("select", (), "s_a", 2, {"field": "a"}),
                    PlanOp("select", (), "s_b", 3, {"field": "b"}),
                    # declared 4 but inputs sum to 5
                    PlanOp("concat", ("s_a", "s_b"), "out", 4),
                ),
                groups={"policy": ("out",)},
                group_widths={"policy": 4},
            )
        )
    assert construct.value.code == "OP_WIDTH"

    # Arrange — construct ok, runtime select field has wrong last dim
    executor = PlanExecutor(
        ObservationPlan(
            state_requirements=(POSE_FIELD,),
            ops=(PlanOp("select", (), "pose", 7, {"field": POSE_FIELD}),),
            groups={"policy": ("pose",)},
            group_widths={"policy": 7},
        )
    )
    wrong = torch.zeros(1, 3)

    # Act / Assert — no silent broadcast
    with pytest.raises(ConfigError) as runtime:
        executor.execute(PlanInputs(raw_state={POSE_FIELD: wrong}))
    assert runtime.value.code == "OP_WIDTH"


def test_ac_py_unit_ops_005_batch_dim_single_and_multi_row() -> None:
    """AC_PY_UNIT_OPS_005: multi-row equals stacked single-row evaluations."""

    # Arrange
    executor = PlanExecutor(_select_concat_plan())
    row0_pose = torch.arange(7, dtype=torch.float32)
    row1_pose = torch.arange(7, dtype=torch.float32) + 10.0
    row0_joint = torch.tensor([0.5])
    row1_joint = torch.tensor([1.5])

    # Act — single-row
    single0 = executor.execute(
        PlanInputs(
            raw_state={
                POSE_FIELD: row0_pose.unsqueeze(0),
                JOINT_FIELD: row0_joint.unsqueeze(0),
            }
        )
    )
    single1 = executor.execute(
        PlanInputs(
            raw_state={
                POSE_FIELD: row1_pose.unsqueeze(0),
                JOINT_FIELD: row1_joint.unsqueeze(0),
            }
        )
    )
    multi = executor.execute(
        PlanInputs(
            raw_state={
                POSE_FIELD: torch.stack([row0_pose, row1_pose]),
                JOINT_FIELD: torch.stack([row0_joint, row1_joint]),
            }
        )
    )

    # Assert shapes
    assert single0["policy"].shape == (1, 8)
    assert multi["policy"].shape == (2, 8)
    # Multi-row rows equal single-row evaluations (catches false broadcast greens)
    assert torch.equal(multi["policy"][0], single0["policy"][0])
    assert torch.equal(multi["policy"][1], single1["policy"][0])
    assert torch.equal(multi["debug"][0], single0["debug"][0])
    assert torch.equal(multi["debug"][1], single1["debug"][0])


def test_empty_plan_returns_empty_groups() -> None:
    """Empty plan (zero ops, zero groups) constructs and returns {}."""

    executor = PlanExecutor(
        ObservationPlan(
            state_requirements=(),
            ops=(),
            groups={},
            group_widths={},
        )
    )
    assert executor.execute(PlanInputs(raw_state={})) == {}


def _previous_action_plan(width: int = 3) -> ObservationPlan:
    return ObservationPlan(
        state_requirements=(),
        ops=(PlanOp("previous_action", (), "prev", width, {"width": width}),),
        groups={"policy": ("prev",)},
        group_widths={"policy": width},
    )


def test_previous_action_first_frame_after_episode_reset_is_zeros() -> None:
    """After episode reset, first execute sees previous_action all zeros."""

    # Arrange — env contract: reset supplies a zero vector, not None
    executor = PlanExecutor(_previous_action_plan(3))
    zeros = torch.zeros(1, 3)

    # Act
    out = executor.execute(PlanInputs(raw_state={}, previous_action=zeros))

    # Assert
    assert torch.equal(out["policy"], zeros)


def test_previous_action_reset_clears_nonzero_residue() -> None:
    """Nonzero previous_action then reset zeros — no residue across episodes."""

    # Arrange
    executor = PlanExecutor(_previous_action_plan(3))
    nonzero = torch.tensor([[0.5, -0.25, 1.0]])
    zeros = torch.zeros(1, 3)

    # Act — write nonzero, then "reset" with zeros
    mid = executor.execute(PlanInputs(raw_state={}, previous_action=nonzero))
    after_reset = executor.execute(PlanInputs(raw_state={}, previous_action=zeros))

    # Assert
    assert torch.equal(mid["policy"], nonzero)
    assert torch.equal(after_reset["policy"], zeros)


def test_command_channel_width_mismatch_fails_at_compile() -> None:
    """Command plan width vs channels() mismatch raises at PlanExecutor construction."""

    plan = ObservationPlan(
        state_requirements=(),
        ops=(PlanOp("command", (), "vel", 3, {"channel": "velocity", "width": 3}),),
        groups={"policy": ("vel",)},
        group_widths={"policy": 3},
    )

    with pytest.raises(ConfigError) as exc_info:
        PlanExecutor(plan, command_channels={"velocity": 2})
    assert exc_info.value.code == "OP_WIDTH"
    assert "velocity" in str(exc_info.value)


def test_command_channel_missing_fails_at_execute() -> None:
    """Missing command channel raises at execute (not compile); message names channel."""

    plan = ObservationPlan(
        state_requirements=(),
        ops=(PlanOp("command", (), "vel", 3, {"channel": "velocity", "width": 3}),),
        groups={"policy": ("vel",)},
        group_widths={"policy": 3},
    )
    executor = PlanExecutor(plan, command_channels={"velocity": 3})

    with pytest.raises(ConfigError) as exc_info:
        executor.execute(PlanInputs(raw_state={}, commands={}))
    assert exc_info.value.code == "OP_MISSING_CHANNEL"
    assert "velocity" in str(exc_info.value)


def _control_frame_dt_plan(scale: float = 100.0, width: int = 1) -> ObservationPlan:
    return ObservationPlan(
        state_requirements=(),
        ops=(PlanOp("control_frame_dt", (), "dt", width, {"scale": scale}),),
        groups={"policy": ("dt",)},
        group_widths={"policy": width},
    )


def test_control_frame_dt_source_is_stateless_and_scaled() -> None:
    """AC-019/022: source maps each supplied interval independently."""

    executor = PlanExecutor(_control_frame_dt_plan())
    first = torch.tensor([[0.005], [0.035]])
    second = torch.tensor([[0.0173], [0.010]])

    assert torch.allclose(
        executor.execute(PlanInputs(raw_state={}, control_frame_dt=first))["policy"],
        torch.tensor([[0.5], [3.5]]),
    )
    assert torch.allclose(
        executor.execute(PlanInputs(raw_state={}, control_frame_dt=second))["policy"],
        torch.tensor([[1.73], [1.0]]),
    )


@pytest.mark.parametrize("scale", [0.0, float("nan"), float("inf")])
def test_control_frame_dt_rejects_invalid_scale_at_compile(scale: float) -> None:
    """AC-020: scale must be finite and non-zero at Compile."""

    with pytest.raises(ConfigError, match="control_frame_dt.*scale"):
        PlanExecutor(_control_frame_dt_plan(scale))


def test_control_frame_dt_missing_scale_rejected_at_compile() -> None:
    plan = ObservationPlan(
        state_requirements=(),
        ops=(PlanOp("control_frame_dt", (), "dt", 1, {}),),
        groups={"policy": ("dt",)},
        group_widths={"policy": 1},
    )
    with pytest.raises(ConfigError, match="scale"):
        PlanExecutor(plan)


@pytest.mark.parametrize(
    "value",
    [None, torch.tensor([[0.0]]), torch.tensor([[-0.005]]), torch.tensor([[float("nan")]])],
)
def test_control_frame_dt_rejects_missing_or_invalid_runtime_value(
    value: torch.Tensor | None,
) -> None:
    """AC-021: missing, non-finite, and non-positive intervals fail at Execute."""

    executor = PlanExecutor(_control_frame_dt_plan())
    with pytest.raises(ConfigError, match="control_frame_dt"):
        executor.execute(PlanInputs(raw_state={}, control_frame_dt=value))


def test_control_frame_dt_rejects_bad_runtime_shape() -> None:
    executor = PlanExecutor(_control_frame_dt_plan())
    with pytest.raises(ConfigError, match="control_frame_dt"):
        executor.execute(PlanInputs(raw_state={}, control_frame_dt=torch.tensor([0.005])))


def test_control_frame_dt_batch_mismatch_is_rejected() -> None:
    plan = ObservationPlan(
        state_requirements=("state",),
        ops=(
            PlanOp("select", (), "state_value", 1, {"field": "state"}),
            PlanOp("control_frame_dt", (), "dt", 1, {"scale": 100.0}),
            PlanOp("concat", ("state_value", "dt"), "out", 2),
        ),
        groups={"policy": ("out",)},
        group_widths={"policy": 2},
    )
    executor = PlanExecutor(plan)
    with pytest.raises(ConfigError, match="batch"):
        executor.execute(
            PlanInputs(
                raw_state={"state": torch.zeros(2, 1)},
                control_frame_dt=torch.full((1, 1), 0.005),
            )
        )

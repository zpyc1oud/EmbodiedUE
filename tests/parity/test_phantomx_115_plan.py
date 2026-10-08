"""PhantomX policy observation plan vs handwritten expected (ticket 11).

The handwritten 116-wide vector lost its contact-force slice (columns 54:61).
The stored ``expected`` is that 109-wide actor vector.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
import torch

from tests.python.unit.robot_shape_fixtures import generic_robot_observation_shapes
from uerl.core.mdp.executor import PlanExecutor, PlanInputs
from uerl.core.mdp.plan import ObservationPlan
from uerl.tasks.phantomx.observation_plan import build_phantomx_observation_plan

_CASES = Path(__file__).resolve().parent / "cases" / "phantomx_115"
_HANDWRITTEN_TOL = 1.0e-6
_SHAPES = generic_robot_observation_shapes()


def _case_files() -> list[Path]:
    return sorted(_CASES.glob("recorded_*.json"))


def test_phantomx_115_corpus_present() -> None:
    assert _CASES.is_dir()
    assert len(_case_files()) == 5


def test_plan_has_root_relative_contract_and_width_109() -> None:
    plan = build_phantomx_observation_plan(_SHAPES)
    assert plan.group_widths["policy"] == 109
    assert "contact_forces" not in plan.groups["policy"]
    assert not any("contact_force" in name for name in plan.state_requirements)
    assert plan.groups["policy"][-1] == "control_frame_dt"
    dt = next(op for op in plan.ops if op.op == "control_frame_dt")
    assert dt.width == 1
    assert dict(dt.params) == {"scale": 100.0}
    assert "offset_default" not in {op.op for op in plan.ops}
    # Field selection is by name; slice start is a structural param, not a bare
    # offset into a flattened Worker blob.
    assert all(
        (op.op != "select") or isinstance(op.params.get("field"), str) for op in plan.ops
    )


@pytest.mark.parametrize(
    ("case_path", "ac"),
    [
        (_CASES / "recorded_flat_01.json", "AC_PARITY_PLAN_001"),
        (_CASES / "recorded_contact_02.json", "AC_PARITY_PLAN_002"),
        (_CASES / "recorded_airborne_03.json", "AC_PARITY_PLAN_003"),
        (_CASES / "recorded_tilted_04.json", "AC_PARITY_PLAN_004"),
        (_CASES / "recorded_moving_05.json", "AC_PARITY_PLAN_004"),
    ],
    ids=lambda value: value if isinstance(value, str) else value.stem,
)
def test_ac_parity_plan_python_matches_handwritten(case_path: Path, ac: str) -> None:
    """AC_PARITY_PLAN_001–004: policy output equals the handwritten vector without contact force."""

    del ac
    payload = json.loads(case_path.read_text(encoding="utf-8"))
    contacts = payload["contacts"]
    on = sum(1 for value in contacts if value > 0.5)
    if case_path.name == "recorded_flat_01.json":
        assert on == len(contacts), "AC_001 requires flat standing (all feet down)"
    if case_path.name == "recorded_contact_02.json":
        assert on == len(contacts), "AC_002 requires multi-foot contact"
    if case_path.name == "recorded_airborne_03.json":
        assert 0 < on < len(contacts), "AC_003 requires partial foot contact"

    plan = ObservationPlan.from_json(payload["plan"])
    assert plan == build_phantomx_observation_plan(_SHAPES)

    raw_state: dict[str, torch.Tensor] = {}
    offset = 0
    for entry in payload["state_layout"]:
        width = int(entry["width"])
        chunk = payload["raw_state"][offset : offset + width]
        raw_state[str(entry["name"])] = torch.tensor([chunk], dtype=torch.float32)
        offset += width

    commands = {
        name: torch.tensor([values], dtype=torch.float32)
        for name, values in payload["commands"].items()
    }
    previous = torch.tensor([payload["previous_action"]], dtype=torch.float32)
    actual = (
        PlanExecutor(plan, command_channels={"velocity": 3})
        .execute(
            PlanInputs(
                raw_state=raw_state,
                commands=commands,
                control_frame_dt=torch.tensor([[payload["control_frame_dt"]]], dtype=torch.float32),
                previous_action=previous,
            )
        )[
            "policy"
        ]
        .reshape(-1)
    )
    expected = torch.tensor(payload["expected"], dtype=torch.float32)
    assert actual.shape == expected.shape == (109,)
    differences = (actual - expected).abs()
    index = int(differences.argmax())
    group_field = "policy"
    local_index = index
    for member in plan.groups["policy"]:
        width = next(op.width for op in plan.ops if op.output == member)
        if local_index < width:
            group_field = member
            break
        local_index -= width
    assert float(differences[index]) <= _HANDWRITTEN_TOL, (
        f"{case_path.name} policy[{index}] {group_field}[{local_index}]: "
        f"actual={float(actual[index])} expected={float(expected[index])} "
        f"absolute tolerance={_HANDWRITTEN_TOL}"
    )

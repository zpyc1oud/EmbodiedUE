"""Cross-language operator parity: Python PlanExecutor vs reviewed expected.

Traverses ``tests/parity/cases/<operator>/*.json`` (skips structure/artifact).
Both sides assert against the reviewed ``expected`` field — not against each other.

Negative cases use ``expected_error`` instead of ``expected``:
``{"phase": "compile"|"execute", "contains": "<substring>"}``.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
import torch

from uerl.core.mdp.executor import PlanExecutor, PlanInputs
from uerl.core.mdp.plan import ObservationPlan
from uerl.errors import ConfigError

_CASES_ROOT = Path(__file__).resolve().parent / "cases"
_SKIP_CATEGORIES = frozenset(
    {
        "structure",
        "artifact",
        "action_plan",
        "deploy",
        "deploy_variable_dt",
        "controller",
        "policynet",
        "phantomx_variants",
    }
)


def _operator_case_files() -> list[Path]:
    """Data-driven discovery — new operator dirs are picked up automatically."""

    if not _CASES_ROOT.is_dir():
        return []
    cases: list[Path] = []
    for category_dir in sorted(p for p in _CASES_ROOT.iterdir() if p.is_dir()):
        if category_dir.name in _SKIP_CATEGORIES:
            continue
        for path in sorted(category_dir.glob("*.json")):
            if path.name.endswith(".expected.json"):
                continue
            cases.append(path)
    return cases


def test_operator_parity_corpus_is_present() -> None:
    """Zero cases must fail — empty traversal would otherwise go green."""

    assert _CASES_ROOT.is_dir(), f"missing parity cases root: {_CASES_ROOT}"
    assert _operator_case_files(), f"no operator parity cases under {_CASES_ROOT}"


def _raw_state_from_layout(
    layout: list[dict[str, object]],
    raw: list[float],
) -> dict[str, torch.Tensor]:
    offset = 0
    mapping: dict[str, torch.Tensor] = {}
    for entry in layout:
        name = entry["name"]
        width = entry["width"]
        assert isinstance(name, str)
        assert isinstance(width, int) and width > 0
        chunk = raw[offset : offset + width]
        if len(chunk) != width:
            raise AssertionError(
                f"raw_state too short for field {name!r}: need {width} from offset {offset}"
            )
        mapping[name] = torch.tensor([chunk], dtype=torch.float32)
        offset += width
    if offset != len(raw):
        raise AssertionError(f"raw_state length {len(raw)} != layout width {offset}")
    return mapping


def _command_channels_from_layout(layout: list[dict[str, object]] | None) -> dict[str, int] | None:
    if layout is None:
        return None
    channels: dict[str, int] = {}
    for entry in layout:
        name = entry["name"]
        width = entry["width"]
        assert isinstance(name, str)
        assert isinstance(width, int) and width > 0
        channels[name] = width
    return channels


@pytest.mark.parametrize("case_path", _operator_case_files(), ids=lambda path: f"{path.parent.name}/{path.stem}")
def test_ac_parity_op_python_matches_expected(case_path: Path) -> None:
    """AC_PARITY_OP_*: PlanExecutor matches reviewed expected, or expected_error."""

    # Arrange — corrupt / missing JSON must fail, not skip
    try:
        payload = json.loads(case_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise AssertionError(f"corrupt JSON in {case_path}: {exc}") from exc

    assert "plan" in payload, f"{case_path.name} missing 'plan'"
    assert "state_layout" in payload, f"{case_path.name} missing 'state_layout'"
    assert "raw_state" in payload, f"{case_path.name} missing 'raw_state'"

    has_expected = "expected" in payload
    has_error = "expected_error" in payload
    assert has_expected ^ has_error, (
        f"{case_path.name} must have exactly one of 'expected' / 'expected_error'"
    )

    plan = ObservationPlan.from_json(payload["plan"])
    raw_state = _raw_state_from_layout(payload["state_layout"], payload["raw_state"])
    command_channels = _command_channels_from_layout(payload.get("command_layout"))

    commands: dict[str, torch.Tensor] = {}
    for name, values in (payload.get("commands") or {}).items():
        commands[name] = torch.tensor([values], dtype=torch.float32)

    previous_action = None
    if "previous_action" in payload and payload["previous_action"] is not None:
        previous_action = torch.tensor([payload["previous_action"]], dtype=torch.float32)

    control_frame_dt = None
    if "control_frame_dt" in payload and payload["control_frame_dt"] is not None:
        control_frame_dt = torch.as_tensor(
            payload["control_frame_dt"], dtype=torch.float32
        ).reshape(-1, 1)

    if has_error:
        error_spec = payload["expected_error"]
        assert isinstance(error_spec, dict), f"{case_path.name} expected_error must be an object"
        phase = error_spec.get("phase")
        contains = error_spec.get("contains")
        assert phase in {"compile", "execute"}, (
            f"{case_path.name} expected_error.phase must be 'compile' or 'execute'"
        )
        assert isinstance(contains, str) and contains, (
            f"{case_path.name} expected_error.contains must be a non-empty string"
        )

        if phase == "compile":
            with pytest.raises(ConfigError) as exc_info:
                PlanExecutor(plan, command_channels=command_channels)
            assert contains in str(exc_info.value), (
                f"{case_path.name}: compile error {exc_info.value!s} missing {contains!r}"
            )
            return

        executor = PlanExecutor(plan, command_channels=command_channels)
        with pytest.raises(ConfigError) as exc_info:
            executor.execute(
                PlanInputs(
                    raw_state=raw_state,
                    commands=commands,
                    control_frame_dt=control_frame_dt,
                    previous_action=previous_action,
                )
            )
        assert contains in str(exc_info.value), (
            f"{case_path.name}: execute error {exc_info.value!s} missing {contains!r}"
        )
        return

    expected = payload["expected"]
    assert isinstance(expected, list), f"{case_path.name} 'expected' must be a list"
    tolerance = float(payload.get("tolerance", 2.0e-5))

    # Act
    groups = PlanExecutor(plan, command_channels=command_channels).execute(
        PlanInputs(
            raw_state=raw_state,
            commands=commands,
            control_frame_dt=control_frame_dt,
            previous_action=previous_action,
        )
    )

    # Assert — policy group vs reviewed expected (not vs C++)
    assert "policy" in groups, f"{case_path.name}: plan must declare a 'policy' group"
    actual = groups["policy"].detach().cpu().reshape(-1).tolist()
    assert len(actual) == len(expected), (
        f"{case_path.name}: width {len(actual)} != expected {len(expected)}"
    )
    for index, (got, want) in enumerate(zip(actual, expected, strict=True)):
        assert abs(got - want) <= tolerance, (
            f"{case_path.name}[{index}]: |{got} - {want}| > {tolerance}"
        )

"""Cross-language action-plan parity: ActionPlanExecutor vs reviewed expected.

Traverses ``tests/parity/cases/action_plan/*.json``.
Both sides assert against the reviewed ``expected`` field — not against each other.

Negative cases use ``expected_error``:
``{"phase": "compile"|"execute", "contains": "<substring>"}``.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
import torch

from uerl.core.mdp.executor import ActionPlanExecutor
from uerl.core.mdp.plan import ActionPlan
from uerl.errors import ConfigError

_CASES_DIR = Path(__file__).resolve().parent / "cases" / "action_plan"


def _action_case_files() -> list[Path]:
    if not _CASES_DIR.is_dir():
        return []
    return sorted(p for p in _CASES_DIR.glob("*.json") if not p.name.endswith(".expected.json"))


def test_action_plan_parity_corpus_is_present() -> None:
    """Zero cases must fail — empty traversal would otherwise go green."""

    assert _CASES_DIR.is_dir(), f"missing action-plan corpus: {_CASES_DIR}"
    assert _action_case_files(), f"no action-plan parity cases under {_CASES_DIR}"


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


@pytest.mark.parametrize("case_path", _action_case_files(), ids=lambda path: path.stem)
def test_ac_parity_action_python_matches_expected(case_path: Path) -> None:
    """AC_PARITY_ACTION_*: ActionPlanExecutor matches reviewed expected, or expected_error."""

    try:
        payload = json.loads(case_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise AssertionError(f"corrupt JSON in {case_path}: {exc}") from exc

    assert "plan" in payload, f"{case_path.name} missing 'plan'"
    assert "policy_action" in payload, f"{case_path.name} missing 'policy_action'"

    has_expected = "expected" in payload
    has_error = "expected_error" in payload
    assert has_expected ^ has_error, (
        f"{case_path.name} must have exactly one of 'expected' / 'expected_error'"
    )

    plan = ActionPlan.from_json(payload["plan"])
    command_channels = _command_channels_from_layout(payload.get("command_layout"))
    policy_action = torch.tensor([payload["policy_action"]], dtype=torch.float32)

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
                ActionPlanExecutor(plan, command_channels=command_channels)
            assert contains in str(exc_info.value), (
                f"{case_path.name}: compile error {exc_info.value!s} missing {contains!r}"
            )
            return

        executor = ActionPlanExecutor(plan, command_channels=command_channels)
        with pytest.raises(ConfigError) as exc_info:
            executor.execute(policy_action)
        assert contains in str(exc_info.value), (
            f"{case_path.name}: execute error {exc_info.value!s} missing {contains!r}"
        )
        return

    expected = payload["expected"]
    assert isinstance(expected, list), f"{case_path.name} 'expected' must be a list"
    tolerance = float(payload.get("tolerance", 1.0e-6))

    commands = ActionPlanExecutor(plan, command_channels=command_channels).execute(policy_action)
    assert len(plan.command_fields) == 1, f"{case_path.name}: corpus expects one command field"
    field = plan.command_fields[0]
    actual = commands[field].detach().cpu().reshape(-1).tolist()
    assert len(actual) == len(expected), (
        f"{case_path.name}: width {len(actual)} != expected {len(expected)}"
    )
    for index, (got, want) in enumerate(zip(actual, expected, strict=True)):
        assert abs(got - want) <= tolerance, (
            f"{case_path.name}[{index}]: |{got} - {want}| > {tolerance}"
        )

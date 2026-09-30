"""Assert Python plan structure matches reviewed parity corpus expectations."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from uerl.core.mdp.plan import ObservationPlan

_CASES_DIR = Path(__file__).resolve().parent / "cases" / "structure"


def _case_files() -> list[Path]:
    return sorted(_CASES_DIR.glob("*.json"))


def test_structure_parity_corpus_is_present() -> None:
    """Corpus absence must fail — empty traversal would otherwise go green."""

    assert _CASES_DIR.is_dir(), f"missing structure corpus directory: {_CASES_DIR}"
    assert _case_files(), f"no structure parity cases under {_CASES_DIR}"


@pytest.mark.parametrize("case_path", _case_files(), ids=lambda path: path.stem)
def test_structure_parity_observation_plan(case_path: Path) -> None:
    """Parsed plan structure matches the reviewed expected descriptor."""

    # Arrange
    payload = json.loads(case_path.read_text(encoding="utf-8"))
    assert "plan" in payload and "expected" in payload

    # Act
    plan = ObservationPlan.from_json(payload["plan"])
    expected = payload["expected"]

    # Assert — compare sequences, not sets, so op order regressions fail.
    assert [op.op for op in plan.ops] == expected["ops"]
    assert [op.width for op in plan.ops] == expected["widths"]
    assert {name: list(members) for name, members in plan.groups.items()} == expected["groups"]

"""End-to-end deploy parity: ReferencePolicyRunner vs reviewed expected corpus.

Traverses ``tests/parity/cases/deploy/<task>/*.json``.
Both Python and UE assert against committed ``expected_action`` /
``expected_targets`` — not against each other.  Targets are flattened in
action-plan command-field order, matching UE's packed physical vector.
"""

from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from pathlib import Path

import numpy as np
import pytest
import torch

from uerl.policy.artifact import PolicyArtifact
from uerl.policy.reference import ReferencePolicyRunner


def _flatten_command_targets(
    result: Mapping[str, torch.Tensor],
    command_fields: Sequence[str],
) -> list[float]:
    """Flatten physical command fields in the action-plan declaration order."""

    values: list[float] = []
    for name in command_fields:
        flat = result[name].detach().cpu().reshape(-1).to(dtype=torch.float32)
        values.extend(float(x) for x in flat.tolist())
    return values


_CASES_ROOT = Path(__file__).resolve().parent / "cases" / "deploy"
_REPO = Path(__file__).resolve().parents[2]
_MIN_STEPS = 3  # AC_003: cover previous_action state advance


def _deploy_case_files() -> list[Path]:
    if not _CASES_ROOT.is_dir():
        return []
    cases: list[Path] = []
    for task_dir in sorted(p for p in _CASES_ROOT.iterdir() if p.is_dir()):
        for path in sorted(task_dir.glob("*.json")):
            cases.append(path)
    return cases


def test_deploy_parity_corpus_is_present() -> None:
    assert _CASES_ROOT.is_dir(), f"missing deploy corpus root: {_CASES_ROOT}"
    assert _deploy_case_files(), f"no deploy parity cases under {_CASES_ROOT}"


def test_ac_parity_deploy_003_multistep_exceeds_history() -> None:
    """AC_PARITY_DEPLOY_003: at least one sequence with >= 3 steps."""

    cases = _deploy_case_files()
    assert cases, "deploy corpus empty"
    longest = 0
    for path in cases:
        payload = json.loads(path.read_text(encoding="utf-8"))
        longest = max(longest, len(payload.get("steps", [])))
    assert longest >= _MIN_STEPS, (
        f"longest deploy sequence has {longest} steps; need >= {_MIN_STEPS} "
        "to exercise previous_action advance"
    )


@pytest.mark.parametrize("case_path", _deploy_case_files(), ids=lambda p: f"{p.parent.name}/{p.stem}")
def test_ac_parity_deploy_001_002_004_python_onnx_matches_expected(case_path: Path) -> None:
    """AC_PARITY_DEPLOY_001/002/004: Python ONNX matches reviewed expected."""

    payload = json.loads(case_path.read_text(encoding="utf-8"))
    assert "artifact" in payload and "steps" in payload
    steps = payload["steps"]
    assert isinstance(steps, list) and len(steps) >= _MIN_STEPS, (
        f"{case_path.name}: need >= {_MIN_STEPS} steps, got {len(steps)}"
    )
    tolerance = float(payload.get("tolerance", 2.0e-5))

    artifact_path = Path(payload["artifact"])
    if not artifact_path.is_absolute():
        artifact_path = (_REPO / artifact_path).resolve()
    artifact = PolicyArtifact.read(artifact_path)
    runner = ReferencePolicyRunner(artifact)
    runner.reset()

    for index, step in enumerate(steps):
        assert "raw_state" in step and "commands" in step
        assert "expected_action" in step and "expected_targets" in step
        result = runner.step(
            {k: torch.tensor([v], dtype=torch.float32) for k, v in step["raw_state"].items()},
            {k: torch.tensor([v], dtype=torch.float32) for k, v in step["commands"].items()},
        )
        action = result["action"].detach().cpu().reshape(-1).tolist()
        targets = _flatten_command_targets(result, artifact.action_plan.command_fields)
        expected_action = step["expected_action"]
        expected_targets = step["expected_targets"]
        assert len(action) == len(expected_action), (
            f"{case_path.name} step {index}: action width {len(action)} != {len(expected_action)}"
        )
        assert len(targets) == len(expected_targets), (
            f"{case_path.name} step {index}: targets width {len(targets)} != {len(expected_targets)}"
        )
        for i, (got, want) in enumerate(zip(action, expected_action, strict=True)):
            assert np.isclose(got, want, atol=tolerance, rtol=tolerance), (
                f"{case_path.name} step {index} action[{i}]: |{got} - {want}| > "
                f"atol/rtol {tolerance}"
            )
        for i, (got, want) in enumerate(zip(targets, expected_targets, strict=True)):
            assert np.isclose(got, want, atol=tolerance, rtol=tolerance), (
                f"{case_path.name} step {index} targets[{i}]: |{got} - {want}| > "
                f"atol/rtol {tolerance}"
            )

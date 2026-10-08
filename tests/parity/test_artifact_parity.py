"""Assert Python artifact reads match the reviewed parity corpus."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest
import torch

from uerl.policy.artifact import PolicyArtifact

_CASES_DIR = Path(__file__).resolve().parent / "cases" / "artifact"


def _artifact_cases() -> list[Path]:
    """Traverse the corpus directory — do not hard-code case names."""

    return sorted(_CASES_DIR.glob("*.uerlpol2"))


def test_artifact_parity_corpus_is_present() -> None:
    """Corpus absence must fail — empty traversal would otherwise go green."""

    assert _CASES_DIR.is_dir(), f"missing artifact corpus directory: {_CASES_DIR}"
    assert _artifact_cases(), f"no *.uerlpol2 cases under {_CASES_DIR}"


@pytest.mark.parametrize("artifact_path", _artifact_cases(), ids=lambda path: path.stem)
def test_artifact_parity_python_matches_expected(artifact_path: Path, tmp_path: Path) -> None:
    """Python read of each committed artifact matches its reviewed expected descriptor."""

    # Arrange
    expected_path = artifact_path.with_suffix(".expected.json")
    assert expected_path.is_file(), f"missing expected descriptor beside {artifact_path.name}"
    expected = json.loads(expected_path.read_text(encoding="utf-8"))

    # Act
    artifact = PolicyArtifact.read(artifact_path)

    # Assert — sequences, not sets; ONNX compared by exact bytes.
    assert artifact.task_id == expected["task_id"]
    assert artifact.robot_id == expected["robot_id"]
    assert [op.op for op in artifact.observation_plan.ops] == expected["observation"]["ops"]
    assert [op.width for op in artifact.observation_plan.ops] == expected["observation"]["widths"]
    assert {
        name: list(members) for name, members in artifact.observation_plan.groups.items()
    } == expected["observation"]["groups"]
    assert dict(artifact.observation_plan.group_widths) == expected["observation"]["group_widths"]
    assert [op.op for op in artifact.action_plan.ops] == expected["action"]["ops"]
    assert [op.width for op in artifact.action_plan.ops] == expected["action"]["widths"]
    assert list(artifact.action_plan.command_fields) == expected["action"]["command_fields"]
    assert artifact.action_plan.policy_width == expected["action"]["policy_width"]
    assert [
        {
            "joint": actuator.joint,
            "stiffness": actuator.stiffness,
            "damping": actuator.damping,
            "effort_limit": actuator.effort_limit,
            "default_position": actuator.default_position,
        }
        for actuator in artifact.robot_runtime.actuators
    ] == expected["robot_runtime"]["actuators"]
    assert len(artifact.onnx) == expected["onnx_nbytes"]
    assert hashlib.sha256(artifact.onnx).hexdigest() == expected["onnx_sha256"]
    assert artifact.timing.to_json() == expected["timing"]
    artifact.validate()

    # Exercise the writer at a separate temporary path, then read what it saved.
    rewritten_path = tmp_path / artifact_path.name
    artifact.write(rewritten_path)
    rewritten = PolicyArtifact.read(rewritten_path)
    assert rewritten.onnx == artifact.onnx
    assert rewritten.observation_plan.to_json() == artifact.observation_plan.to_json()
    assert rewritten.action_plan.to_json() == artifact.action_plan.to_json()
    assert rewritten.robot_runtime.to_json() == expected["robot_runtime"]
    assert rewritten.timing.to_json() == expected["timing"]
    assert rewritten.metadata.to_json() == artifact.metadata.to_json()
    assert (rewritten.task_id, rewritten.robot_id) == (expected["task_id"], expected["robot_id"])


def test_legacy_policynet_generator_preserves_source_timing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The legacy model inherits 5ms decimation1..7; synthetic models retain 1/60."""
    from tests.parity.cases.policynet import generate_fixtures

    monkeypatch.setattr(generate_fixtures, "_OUT", tmp_path)
    with torch.random.fork_rng(devices=[]):
        generate_fixtures.generate_phantomx_fixture()
    artifact = PolicyArtifact.read(tmp_path / "phantomx_115.uerlpol2")
    assert artifact.timing.to_json() == {
        "physics_dt": 0.005, "decimation_min": 1, "decimation_max": 7,
    }
    assert artifact.observation_plan.group_widths["policy"] == 115
    assert artifact.action_plan.policy_width == 18

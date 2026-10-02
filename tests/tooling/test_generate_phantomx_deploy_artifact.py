"""The legacy policy conversion writes an artifact matching its network input."""
from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

from uerl.policy.artifact import ArtifactTiming, PolicyArtifact


def test_legacy_phantomx_generator_preserves_115_input_contract(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    script = Path(__file__).resolve().parents[2] / "scripts/generate_phantomx_deploy_artifact.py"
    spec = importlib.util.spec_from_file_location("generate_phantomx", script)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    output = tmp_path / "legacy.uerlpol2"
    monkeypatch.setattr(module, "_OUT", output)
    module.main()
    artifact = PolicyArtifact.read(output)
    assert artifact.observation_plan.group_widths["policy"] == 115
    assert artifact.timing == ArtifactTiming(0.005, 1, 7)
    assert not list(tmp_path.glob("*.tmp"))
    reference = PolicyArtifact.read(
        script.parents[1] / "engine/Content/UERLHost/Policies/PhantomXContinuousSmooth.uerlpol2"
    )
    assert artifact.observation_plan.groups == reference.observation_plan.groups
    assert {op.output: op for op in artifact.observation_plan.ops} == {
        op.output: op for op in reference.observation_plan.ops
    }

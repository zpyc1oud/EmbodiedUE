"""Paired input contract, policy container and saved Run behavior."""

import json
import struct
from dataclasses import replace
from pathlib import Path
from typing import Any, cast

import pytest

from tests.python.run_config_files import write_resolved_config
from uerl.application.run_config import load_run_config_source
from uerl.core.config.canonical import to_jsonable
from uerl.core.inputs import RayGroundFactory, StateInputFactory
from uerl.core.inputs.serialization import input_contract, parse_input_contract
from uerl.errors import ConfigError
from uerl.policy.artifact import PolicyArtifact
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config

CORPUS = Path(__file__).parents[2] / "parity/cases/inputs/contract.json"


def _make_artifact() -> PolicyArtifact:
    source = PolicyArtifact.read(CORPUS.parent.parent / "controller/cartpole_controller.uerlpol2")
    return replace(source, format_version=2)


def _contract() -> dict[str, Any]:
    return cast(dict[str, Any], json.loads(CORPUS.read_text(encoding="utf-8")))


def test_literal_paired_contract_preserves_scalar_and_ray_meaning(tmp_path: Path) -> None:
    requirements = parse_input_contract(_contract())
    state, ray = requirements
    assert state.fields[0].shape == ()
    assert state.fields[0].width == 1
    assert StateInputFactory(state.fields).validate(state.spec) == state
    assert RayGroundFactory().validate(ray.spec) == ray
    assert input_contract(requirements) == _contract()
    artifact = replace(_make_artifact(), input_requirements=requirements)
    artifact.write(tmp_path / "input.uerlpol2")
    restored = PolicyArtifact.read(tmp_path / "input.uerlpol2")
    assert restored.format_version == 2
    assert restored.input_requirements == requirements
    assert restored.onnx == artifact.onnx


@pytest.mark.parametrize("case", json.loads(CORPUS.with_name("invalid_contracts.json").read_text()),
                         ids=lambda case: case["name"])
def test_persisted_contract_rejects_incomplete_or_changed_schema(case: dict[str, Any]) -> None:
    with pytest.raises(ConfigError):
        parse_input_contract(case["contract"])


def test_container_version_gates_input_contract(tmp_path: Path) -> None:
    artifact = _make_artifact()
    legacy = replace(artifact, format_version=1)
    legacy.write(tmp_path / "legacy")
    artifact.write(tmp_path / "new")
    assert PolicyArtifact.read(tmp_path / "legacy").input_requirements == ()
    assert "input_contract" not in legacy._json_payload()
    # Same legacy JSON under a v2 header cannot silently become an input policy.
    wrong = bytearray((tmp_path / "legacy").read_bytes())
    struct.pack_into("<I", wrong, 8, 2)
    with pytest.raises(ConfigError, match="input_contract"):
        PolicyArtifact.from_bytes(bytes(wrong))
    wrong = bytearray((tmp_path / "new").read_bytes())
    struct.pack_into("<I", wrong, 8, 1)
    with pytest.raises(ConfigError, match="input_contract"):
        PolicyArtifact.from_bytes(bytes(wrong))


@pytest.mark.parametrize("legacy", [False, True])
def test_saved_run_restores_declarations_without_mutating_snapshot(tmp_path: Path, legacy: bool) -> None:
    specs = [row["spec"] for row in _contract()["requirements"]]
    config = build_run_config(CARTPOLE_TASK_ID, overrides={"worker.inputs": json.dumps(specs)})
    payload = to_jsonable(config)
    if legacy:
        del payload["worker"]["inputs"]
    path = write_resolved_config(tmp_path, payload)
    original = path.read_bytes()
    restored = load_run_config_source(None, tmp_path, strict=True).resolve({}).config
    assert restored.worker.inputs == (() if legacy else config.worker.inputs)
    assert path.read_bytes() == original

"""Sequential deploy parity for the variable control-frame contract."""

from __future__ import annotations

import json
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import cast

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


_CASE_ROOT = Path(__file__).resolve().parent / "cases" / "deploy_variable_dt"
_REPO = Path(__file__).resolve().parents[2]
_CASE = _CASE_ROOT / "phantomx_variable_dt.json"


def _load() -> tuple[dict[str, object], PolicyArtifact]:
    payload = json.loads(_CASE.read_text(encoding="utf-8"))
    artifact_path = Path(str(payload["artifact"]))
    if not artifact_path.is_absolute():
        artifact_path = (_REPO / artifact_path).resolve()
    return payload, PolicyArtifact.read(artifact_path)


def test_ac_parity_deploy_variable_dt_corpus_is_present() -> None:
    assert _CASE_ROOT.is_dir()
    assert _CASE.is_file()
    payload = json.loads(_CASE.read_text(encoding="utf-8"))
    assert len(payload.get("steps", [])) >= 5


def test_ac_parity_deploy_variable_dt_005_006_007_python_reference() -> None:
    """Observation/action parity covers off-grid dt, history overrides, and reset."""

    payload, artifact = _load()
    tolerance = float(cast(float, payload["tolerance"]))
    required = {0.005, 0.010, 0.0173, 0.035}
    steps = payload["steps"]
    assert isinstance(steps, list)
    assert required.issubset({float(step["control_frame_dt"]) for step in steps})
    assert any(bool(step["history_reset"]) for step in steps)
    assert any("previous_action" in step for step in steps)

    runner = ReferencePolicyRunner(artifact)
    runner.reset()
    for index, step in enumerate(steps):
        assert isinstance(step, dict)
        result = runner.step(
            {
                name: torch.tensor([values], dtype=torch.float32)
                for name, values in step["raw_state"].items()
            },
            {
                name: torch.tensor([values], dtype=torch.float32)
                for name, values in step["commands"].items()
            },
            control_frame_dt=float(step["control_frame_dt"]),
            reset_history=bool(step["history_reset"]),
            previous_action=(
                torch.tensor([step["previous_action"]], dtype=torch.float32)
                if "previous_action" in step
                else None
            ),
        )
        for key in ("observation", "action"):
            actual = result[key].detach().cpu().reshape(-1).numpy()
            expected = np.asarray(step[f"expected_{key}"], dtype=np.float32)
            np.testing.assert_allclose(actual, expected, atol=tolerance, rtol=tolerance)
        targets = _flatten_command_targets(result, artifact.action_plan.command_fields)
        np.testing.assert_allclose(
            targets,
            np.asarray(step["expected_targets"], dtype=np.float32),
            atol=tolerance,
            rtol=tolerance,
            err_msg=f"step {index} targets",
        )


@pytest.mark.parametrize("dt", [0.0173, 0.035])
def test_ac_parity_deploy_variable_dt_007_does_not_quantize_dt(dt: float) -> None:
    """The exported plan preserves an off-grid interval as a scaled scalar."""

    _payload, artifact = _load()
    runner = ReferencePolicyRunner(artifact)
    step = json.loads(_CASE.read_text(encoding="utf-8"))["steps"][0]
    result = runner.step(
        {name: torch.tensor([values], dtype=torch.float32) for name, values in step["raw_state"].items()},
        {name: torch.tensor([values], dtype=torch.float32) for name, values in step["commands"].items()},
        control_frame_dt=dt,
        reset_history=True,
    )
    assert float(result["observation"][0, -1]) == pytest.approx(dt * 100.0)

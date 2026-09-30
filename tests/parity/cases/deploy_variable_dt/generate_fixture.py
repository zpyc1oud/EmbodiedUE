"""Generate the reviewed variable-dt deploy parity fixture.

The fixture is deliberately a small deterministic contract probe. It is not a
replacement for the PhantomX retrained artifact; its purpose is to exercise the
new 116-wide plan, history reset, and off-grid control intervals in both runtimes.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import cast

import torch
from torch import nn

from uerl.core.mdp.plan import ActionPlan
from uerl.policy.artifact import ArtifactMetadata, PolicyArtifact
from uerl.policy.reference import ReferencePolicyRunner
from uerl.tasks.phantomx.observation_plan import build_phantomx_observation_plan

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))

from tests.python.unit.robot_shape_fixtures import generic_robot_observation_shapes  # noqa: E402

OLD_CASE = ROOT / "tests" / "parity" / "cases" / "deploy" / "phantomx" / "multistep_walk.json"
OLD_ARTIFACT = ROOT / "engine" / "Content" / "UERLHost" / "Policies" / "PhantomXContinuousSmooth.uerlpol2"
OUT = Path(__file__).resolve().parent
ARTIFACT_PATH = OUT / "phantomx_variable_dt.uerlpol2"
CASE_PATH = OUT / "phantomx_variable_dt.json"


class ContractProbe(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.linear = nn.Linear(116, 18)
        with torch.no_grad():
            self.linear.weight.zero_()
            self.linear.bias.copy_(torch.linspace(-0.09, 0.09, 18))
            for index in range(18):
                self.linear.weight[index, index] = 0.003 * (index + 1)
                self.linear.weight[index, -1] = 0.001 * (index + 1)

    def forward(self, observation: torch.Tensor) -> torch.Tensor:
        return cast(torch.Tensor, self.linear(observation))


def export_onnx() -> bytes:
    model = ContractProbe().eval()
    path = OUT / "_phantomx_variable_dt.onnx"
    torch.onnx.export(
        model,
        (torch.zeros(1, 116, dtype=torch.float32),),
        str(path),
        input_names=["obs"],
        output_names=["actions"],
        dynamo=False,
        opset_version=13,
    )
    data: bytes = path.read_bytes()
    path.unlink(missing_ok=True)
    return bytes(data)


def main() -> None:
    source = json.loads(OLD_CASE.read_text(encoding="utf-8"))
    old = PolicyArtifact.read(OLD_ARTIFACT)
    plan = build_phantomx_observation_plan(generic_robot_observation_shapes())
    artifact = PolicyArtifact(
        format_version=old.format_version,
        task_id="UERL-PhantomX-VariableDt-ContractFixture-v0",
        robot_id=old.robot_id,
        observation_plan=plan,
        action_plan=ActionPlan.from_json(old.action_plan.to_json()),
        robot_runtime=old.robot_runtime,
        timing=old.timing,
        onnx=export_onnx(),
        metadata=ArtifactMetadata(
            run_hash="not-recorded",
            git_identity={"commit": "ticket11-contract-fixture", "ref": "local", "dirty": True},
        ),
    )
    artifact.validate()
    artifact.write(ARTIFACT_PATH)

    dts = [0.005, 0.0173, 0.035, 0.010, 0.005]
    source_steps = cast(list[dict[str, object]], source["steps"])
    steps: list[dict[str, object]] = []
    for index, dt in enumerate(dts):
        source_step = source_steps[index % len(source_steps)]
        step: dict[str, object] = {
            "raw_state": source_step["raw_state"],
            "commands": source_step["commands"],
            "control_frame_dt": dt,
            "history_reset": index in (0, 4),
        }
        if index == 2:
            # Explicit history is part of the reviewed input schema; the value
            # differs from the automatically previous action on purpose.
            step["previous_action"] = [0.05] * 18
        steps.append(step)

    runner = ReferencePolicyRunner(artifact)
    runner.reset()
    for step in steps:
        raw_state = cast(dict[str, list[float]], step["raw_state"])
        commands = cast(dict[str, list[float]], step["commands"])
        result = runner.step(
            {name: torch.tensor([values], dtype=torch.float32) for name, values in raw_state.items()},
            {name: torch.tensor([values], dtype=torch.float32) for name, values in commands.items()},
            control_frame_dt=float(cast(float, step["control_frame_dt"])),
            reset_history=bool(step["history_reset"]),
            previous_action=(
                torch.tensor([cast(list[float], step["previous_action"])], dtype=torch.float32)
                if "previous_action" in step
                else None
            ),
        )
        step["expected_observation"] = result["observation"].reshape(-1).tolist()
        step["expected_action"] = result["action"].reshape(-1).tolist()
        step["expected_targets"] = result["target"].reshape(-1).tolist()

    payload = {
        "notes": "AC_PARITY_DEPLOY_005/006/007: 116-wide PhantomX plan, explicit dt/history contract probe",
        "artifact": str(ARTIFACT_PATH.relative_to(ROOT)).replace("\\", "/"),
        "tolerance": 2.0e-5,
        "required_dt_values": [0.005, 0.010, 0.0173, 0.035],
        "steps": steps,
    }
    CASE_PATH.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {ARTIFACT_PATH}")
    print(f"wrote {CASE_PATH}")


if __name__ == "__main__":
    main()

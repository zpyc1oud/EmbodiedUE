"""Repackage the reviewed controller ONNX with explicit state and ground inputs."""

import json
from dataclasses import replace
from pathlib import Path

from uerl import UERLDirectEnv  # noqa: F401 -- initialize the public Direct API used by plans
from uerl.core.inputs.serialization import parse_input_contract
from uerl.core.mdp.plan import ObservationPlan, PlanOp
from uerl.policy.artifact import PolicyArtifact


def generate(directory: Path) -> None:
    source = PolicyArtifact.read(directory.parent / "controller/cartpole_controller.uerlpol2")
    contract = json.loads((directory / "contract.json").read_text(encoding="utf-8"))
    observation = ObservationPlan(
        state_requirements=("input.ground.clearance", "robot.joint.pole.joint_position"),
        ops=(
            PlanOp(op="select", inputs=(), output="cart_pos", width=1, params={"field": "input.ground.clearance"}),
            *source.observation_plan.ops[1:],
        ),
        groups=source.observation_plan.groups,
        group_widths=source.observation_plan.group_widths,
    )
    replace(source, format_version=2, observation_plan=observation,
            input_requirements=parse_input_contract(contract)).write(directory / "controller_inputs.uerlpol2")


if __name__ == "__main__":
    generate(Path(__file__).parent)

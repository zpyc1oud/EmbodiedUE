"""Derive AC_022's no-command CartPole fixture from the reviewed controller model."""

from dataclasses import replace
from pathlib import Path

from uerl.core.mdp.plan import PlanOp
from uerl.policy.artifact import PolicyArtifact


def main() -> None:
    directory = Path(__file__).resolve().parent
    source = PolicyArtifact.read(directory / "cartpole_controller.uerlpol2")
    field = "robot.joint.cart.joint_velocity"
    # Keep the existing three-input ONNX model and replace its command input
    # with a real CartPole state field, so the controller needs no commands.
    ops = tuple(
        PlanOp("select", (), op.output, op.width, {"field": field})
        if op.op == "command" else op
        for op in source.observation_plan.ops
    )
    plan = replace(
        source.observation_plan,
        ops=ops,
        state_requirements=(*source.observation_plan.state_requirements, field),
    )
    replace(source, observation_plan=plan).write(directory / "cartpole_no_commands.uerlpol2")


if __name__ == "__main__":
    main()

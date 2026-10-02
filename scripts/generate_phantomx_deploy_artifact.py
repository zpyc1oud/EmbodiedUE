"""Convert the legacy 115-input PhantomX policy to a deploy artifact.

Preserves the historical contact-force inputs and timing of the bundled policy.
New training runs should use the standard ``uerl export`` workflow.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import replace
from pathlib import Path

import numpy as np
import torch
from torch import nn

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "src"))

from uerl import ObservationShapeTable, ObsType  # noqa: E402
from uerl.core.mdp.plan import PLAN_VERSION, ActionPlan, ObservationPlan, PlanOp  # noqa: E402
from uerl.policy.artifact import (  # noqa: E402
    ARTIFACT_FORMAT_VERSION,
    ArtifactMetadata,
    ArtifactTiming,
    PolicyArtifact,
    RobotRuntime,
    RobotRuntimeActuator,
)
from uerl.tasks.phantomx.config import PHANTOMX_FEET, PHANTOMX_JOINTS  # noqa: E402
from uerl.tasks.phantomx.observation_plan import (  # noqa: E402
    PHANTOMX_JOINT_DEFAULTS,
    build_phantomx_observation_plan,
)

_MLP1 = REPO / "engine/Content/UERLHost/Policies/PhantomXContinuousSmooth.uerlpolicy"
_OUT = REPO / "engine/Content/UERLHost/Policies/PhantomXContinuousSmooth.uerlpol2"
_NORM_EPS = 0.01
_LEGACY_SHAPES = ObservationShapeTable(  # Legacy 115-wide artifact input.
    {
        ObsType.JOINT_POSITION: (),
        ObsType.JOINT_VELOCITY: (),
        ObsType.BODY_POSE: (7,),
        ObsType.BODY_LINEAR_VELOCITY: (3,),
        ObsType.BODY_ANGULAR_VELOCITY: (3,),
        ObsType.GROUND_CLEARANCE: (),
        ObsType.CONTACT: (),
        ObsType.CONTACT_FORCE: (),
        ObsType.TERRAIN_HEIGHT: (35,),
    }
)


def _parse_uerlmlp1(path: Path) -> tuple[np.ndarray, np.ndarray, list[tuple[np.ndarray, np.ndarray]]]:
    raw = path.read_bytes()
    if raw[:9] != b"UERLMLP1\x00":
        raise ValueError(f"bad magic {raw[:9]!r}")
    offset = 9
    version, in_width, layer_count = struct.unpack_from("<III", raw, offset)
    offset += 12
    if version != 1 or in_width != 115 or layer_count != 4:
        raise ValueError(f"unexpected header {version=} {in_width=} {layer_count=}")
    mean = np.frombuffer(raw, dtype=np.float32, count=in_width, offset=offset).copy()
    offset += in_width * 4
    std = np.frombuffer(raw, dtype=np.float32, count=in_width, offset=offset).copy()
    offset += in_width * 4
    layers: list[tuple[np.ndarray, np.ndarray]] = []
    prev = in_width
    for layer_index in range(layer_count):
        lin, lout = struct.unpack_from("<II", raw, offset)
        offset += 8
        if lin != prev:
            raise ValueError(f"layer {layer_index} input mismatch")
        weights = np.frombuffer(raw, dtype=np.float32, count=lout * lin, offset=offset).copy()
        offset += lout * lin * 4
        bias = np.frombuffer(raw, dtype=np.float32, count=lout, offset=offset).copy()
        offset += lout * 4
        layers.append((weights.reshape(lout, lin), bias))
        prev = lout
    if offset != len(raw):
        raise ValueError(f"trailing bytes: {len(raw) - offset}")
    return mean, std, layers


class _PhantomXOnnx(nn.Module):
    def __init__(
        self,
        mean: np.ndarray,
        std: np.ndarray,
        layers: list[tuple[np.ndarray, np.ndarray]],
    ) -> None:
        super().__init__()
        self.register_buffer("mean", torch.tensor(mean, dtype=torch.float32))
        self.register_buffer("std", torch.tensor(std, dtype=torch.float32))
        modules: list[nn.Module] = []
        for index, (weight, bias) in enumerate(layers):
            linear = nn.Linear(weight.shape[1], weight.shape[0])
            with torch.no_grad():
                linear.weight.copy_(torch.tensor(weight, dtype=torch.float32))
                linear.bias.copy_(torch.tensor(bias, dtype=torch.float32))
            modules.append(linear)
            if index < len(layers) - 1:
                modules.append(nn.ELU())
        self.net = nn.Sequential(*modules)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        mean = self.mean
        std = self.std
        assert isinstance(mean, torch.Tensor) and isinstance(std, torch.Tensor)
        normalized = (x - mean) / (std + _NORM_EPS)
        out = self.net(normalized)
        assert isinstance(out, torch.Tensor)
        return out


def _legacy_observation_plan() -> ObservationPlan:
    plan = build_phantomx_observation_plan(_LEGACY_SHAPES)
    ops = [op for op in plan.ops if op.output != "control_frame_dt"]
    force_bodies = ("base_link", *PHANTOMX_FEET)
    force_slots = ("force_base", *(f"force_{foot}" for foot in PHANTOMX_FEET))
    force_fields = tuple(f"robot.body.{body}.contact_force" for body in force_bodies)
    ops.extend(
        PlanOp("select", (), slot, 1, {"field": field})
        for slot, field in zip(force_slots, force_fields, strict=True)
    )
    ops.append(PlanOp("concat", force_slots, "contact_forces", 7, {}))
    group = tuple(member for member in plan.groups["policy"] if member != "control_frame_dt")
    contact_index = group.index("contacts") + 1
    group = (*group[:contact_index], "contact_forces", *group[contact_index:])
    return replace(
        plan,
        ops=tuple(ops),
        state_requirements=(*plan.state_requirements, *force_fields),
        groups={"policy": group},
        group_widths={"policy": 115},
    )


def _action_plan() -> ActionPlan:
    return ActionPlan(
        ops=(
            PlanOp("policy_action", (), "raw", 18, {"width": 18}),
            PlanOp("clip", ("raw",), "clipped", 18, {"low": -1.0, "high": 1.0}),
            PlanOp("scale", ("clipped",), "scaled", 18, {"factor": 0.20}),
            PlanOp("offset", ("scaled",), "target", 18, {"bias": PHANTOMX_JOINT_DEFAULTS}),
        ),
        command_fields=("target",),
        policy_width=18,
        plan_version=PLAN_VERSION,
    )


def _robot_runtime() -> RobotRuntime:
    return RobotRuntime(
        actuators=tuple(
            RobotRuntimeActuator(
                joint=joint,
                stiffness=25.0,
                damping=0.5,
                effort_limit=2.8,
                default_position=PHANTOMX_JOINT_DEFAULTS[index],
            )
            for index, joint in enumerate(PHANTOMX_JOINTS)
        )
    )


def main() -> None:
    mean, std, layers = _parse_uerlmlp1(_MLP1)
    model = _PhantomXOnnx(mean, std, layers)
    model.eval()
    buffer = torch.zeros(1, 115, dtype=torch.float32)
    onnx_path = _OUT.with_suffix(".onnx.tmp")
    torch.onnx.export(
        model,
        (buffer,),
        str(onnx_path),
        input_names=["obs"],
        output_names=["actions"],
        dynamo=False,
        opset_version=17,
    )
    onnx_bytes = onnx_path.read_bytes()
    onnx_path.unlink(missing_ok=True)

    artifact = PolicyArtifact(
        format_version=ARTIFACT_FORMAT_VERSION,
        task_id="phantomx.pursuit",
        robot_id="phantomx",
        observation_plan=_legacy_observation_plan(),
        action_plan=_action_plan(),
        robot_runtime=_robot_runtime(),
        onnx=onnx_bytes,
        timing=ArtifactTiming(0.005, 1, 7),
        metadata=ArtifactMetadata(
            run_hash="0" * 64,
            git_identity={"commit": "deploy", "ref": "local", "dirty": True},
        ),
    )
    artifact.validate()
    artifact.write(_OUT)
    print(f"wrote {_OUT} ({_OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()

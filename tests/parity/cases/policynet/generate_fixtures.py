"""Generate ONNX / UERLPOL2 fixtures for UE policy-network automation tests.

Produces:
  - mlp_2x64.uerlpol2          — 2-layer MLP, hidden 64 (AC_001 / AC_002)
  - mlp_5x256.uerlpol2         — 5-layer MLP, hidden 256 (AC_002)
  - mlp_norm_nontrivial.uerlpol2 + expected.json  — mean≠0, std≠1 (AC_003)
  - phantomx_115.uerlpol2 + expected.json — UERLMLP1→ONNX parity (2e-5)

Run from repo root:
  .venv/Scripts/python.exe tests/parity/cases/policynet/generate_fixtures.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

from uerl.core.mdp.plan import PLAN_VERSION, ActionPlan, ObservationPlan, PlanOp
from uerl.policy.artifact import (
    ARTIFACT_FORMAT_VERSION,
    ArtifactMetadata,
    ArtifactTiming,
    PolicyArtifact,
    RobotRuntime,
    RobotRuntimeActuator,
)

_OUT = Path(__file__).resolve().parent
_PHANTOMX_POLICY = (
    Path(__file__).resolve().parents[4]
    / "engine"
    / "Content"
    / "UERLHost"
    / "Policies"
    / "PhantomXContinuousSmooth.uerlpolicy"
)

_PHANTOMX_EXPECTED = [
    -1.86217892,
    0.409937501,
    0.238430738,
    0.274483144,
    -1.37132633,
    -2.4545927,
    -0.12709032,
    0.07096266,
    1.78657484,
    2.30448246,
    1.38961422,
    -1.43666971,
    -2.42620277,
    -0.928907812,
    1.4647702,
    0.47180903,
    -0.994486749,
    2.43046641,
]

_NORM_EPS = 0.01
_SYNTHETIC_TIMING = ArtifactTiming(1.0 / 60.0, 1, 1)


def _metadata() -> ArtifactMetadata:
    return ArtifactMetadata(
        run_hash="b" * 64,
        git_identity={"commit": "policynet", "ref": "fixtures", "dirty": False},
    )


def _observation_plan(width: int) -> ObservationPlan:
    return ObservationPlan(
        state_requirements=("robot.joint.fixture.joint_position",),
        ops=(
            PlanOp(
                op="select",
                inputs=(),
                output="joint",
                width=width,
                params={"field": "robot.joint.fixture.joint_position"},
            ),
        ),
        groups={"policy": ("joint",)},
        group_widths={"policy": width},
        plan_version=PLAN_VERSION,
    )


def _action_plan(width: int) -> ActionPlan:
    return ActionPlan(
        ops=(
            PlanOp(op="policy_action", inputs=(), output="raw", width=width),
            PlanOp(
                op="clip",
                inputs=("raw",),
                output="cmd",
                width=width,
                params={"low": -1.0, "high": 1.0},
            ),
        ),
        command_fields=("cmd",),
        policy_width=width,
        plan_version=PLAN_VERSION,
    )


def _robot_runtime(*, actuator_count: int) -> RobotRuntime:
    return RobotRuntime(
        actuators=tuple(
            RobotRuntimeActuator(
                joint=f"fixture_joint_{index}",
                stiffness=25.0,
                damping=0.5,
                effort_limit=10.0,
                default_position=0.0,
            )
            for index in range(actuator_count)
        )
    )


def _write_artifact(
    path: Path, *, obs: int, act: int, onnx_bytes: bytes,
    timing: ArtifactTiming = _SYNTHETIC_TIMING,
) -> None:
    artifact = PolicyArtifact(
        format_version=ARTIFACT_FORMAT_VERSION,
        task_id="policynet.fixture",
        robot_id="fixture",
        observation_plan=_observation_plan(obs),
        action_plan=_action_plan(act),
        robot_runtime=_robot_runtime(actuator_count=act),
        onnx=onnx_bytes,
        metadata=_metadata(),
        timing=timing,
    )
    artifact.write(path)


class _NormMLP(nn.Module):
    """MLP with observation normalizer baked into the forward (matches RSL-RL ONNX)."""

    def __init__(
        self,
        in_width: int,
        out_width: int,
        hidden: int,
        n_layers: int,
        mean: torch.Tensor,
        std: torch.Tensor,
        eps: float = _NORM_EPS,
    ) -> None:
        super().__init__()
        self.register_buffer("mean", mean.clone())
        self.register_buffer("std", std.clone())
        self.eps = eps
        layers: list[nn.Module] = []
        width = in_width
        for index in range(n_layers):
            out = out_width if index == n_layers - 1 else hidden
            layers.append(nn.Linear(width, out))
            if index < n_layers - 1:
                layers.append(nn.ELU())
            width = out
        self.net = nn.Sequential(*layers)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        mean = self.mean
        std = self.std
        assert isinstance(mean, torch.Tensor) and isinstance(std, torch.Tensor)
        x = (x - mean) / (std + self.eps)
        out = self.net(x)
        assert isinstance(out, torch.Tensor)
        return out


def _export_onnx(model: nn.Module, in_width: int) -> bytes:
    model.eval()
    dummy = torch.zeros(1, in_width, dtype=torch.float32)
    path = _OUT / "_tmp_export.onnx"
    torch.onnx.export(
        model,
        (dummy,),
        str(path),
        input_names=["obs"],
        output_names=["actions"],
        dynamo=False,
        opset_version=13,
    )
    data = path.read_bytes()
    path.unlink(missing_ok=True)
    return data


def _seeded_mlp(
    *,
    in_width: int,
    out_width: int,
    hidden: int,
    n_layers: int,
    seed: int,
    mean: np.ndarray | None = None,
    std: np.ndarray | None = None,
) -> tuple[_NormMLP, bytes]:
    torch.manual_seed(seed)
    mean_t = torch.tensor(
        mean if mean is not None else np.zeros(in_width, dtype=np.float32),
        dtype=torch.float32,
    )
    std_t = torch.tensor(
        std if std is not None else np.ones(in_width, dtype=np.float32),
        dtype=torch.float32,
    )
    model = _NormMLP(in_width, out_width, hidden, n_layers, mean_t, std_t)
    # Deterministic nontrivial weights after construction.
    with torch.no_grad():
        for param in model.net.parameters():
            param.normal_(0.0, 0.05)
    return model, _export_onnx(model, in_width)


def _parse_uerlmlp1(path: Path) -> tuple[np.ndarray, np.ndarray, list[tuple[np.ndarray, np.ndarray]]]:
    raw = path.read_bytes()
    # C++ uses ANSICHAR PolicyMagic[] = "UERLMLP1" — includes the trailing NUL.
    magic = raw[:9]
    if magic != b"UERLMLP1\x00":
        raise ValueError(f"bad magic {magic!r}")
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


class _PhantomXmlOnnx(nn.Module):
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
        x = (x - mean) / (std + _NORM_EPS)
        out = self.net(x)
        assert isinstance(out, torch.Tensor)
        return out


def generate_phantomx_fixture() -> None:
    """Convert the legacy model while preserving its recorded deployment timing."""
    # PhantomX parity — convert legacy UERLMLP1 to ONNX with baked normalizer.
    if not _PHANTOMX_POLICY.is_file():
        raise FileNotFoundError(_PHANTOMX_POLICY)
    mean_p, std_p, layers = _parse_uerlmlp1(_PHANTOMX_POLICY)
    phantom = _PhantomXmlOnnx(mean_p, std_p, layers)
    onnx_p = _export_onnx(phantom, 115)
    with torch.no_grad():
        actual = phantom(torch.zeros(1, 115)).numpy().reshape(-1)
    expected_p = np.asarray(_PHANTOMX_EXPECTED, dtype=np.float32)
    max_err = float(np.max(np.abs(actual - expected_p)))
    if max_err > 2.0e-5:
        raise RuntimeError(f"PhantomX torch/ONNX reference max abs err {max_err} > 2e-5")
    source_timing = PolicyArtifact.read(_PHANTOMX_POLICY.with_suffix(".uerlpol2")).timing
    _write_artifact(
        _OUT / "phantomx_115.uerlpol2", obs=115, act=18, onnx_bytes=onnx_p,
        timing=source_timing,
    )
    (_OUT / "phantomx_115.expected.json").write_text(
        json.dumps(
            {
                "input": [0.0] * 115,
                "expected": _PHANTOMX_EXPECTED,
                "tolerance": 2.0e-5,
                "max_torch_abs_err": max_err,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def main() -> None:
    _OUT.mkdir(parents=True, exist_ok=True)

    # AC_001 / AC_002 — two differently shaped MLPs (identity-ish normalizer).
    model_2x64, onnx_2x64 = _seeded_mlp(
        in_width=8, out_width=4, hidden=64, n_layers=2, seed=201
    )
    _write_artifact(_OUT / "mlp_2x64.uerlpol2", obs=8, act=4, onnx_bytes=onnx_2x64)
    with torch.no_grad():
        out_2 = model_2x64(torch.zeros(1, 8)).numpy().reshape(-1).tolist()
    (_OUT / "mlp_2x64.expected.json").write_text(
        json.dumps({"input": [0.0] * 8, "expected": out_2, "tolerance": 2.0e-5}, indent=2)
        + "\n",
        encoding="utf-8",
    )

    model_5x256, onnx_5x256 = _seeded_mlp(
        in_width=16, out_width=6, hidden=256, n_layers=5, seed=505
    )
    _write_artifact(_OUT / "mlp_5x256.uerlpol2", obs=16, act=6, onnx_bytes=onnx_5x256)
    with torch.no_grad():
        out_5 = model_5x256(torch.zeros(1, 16)).numpy().reshape(-1).tolist()
    (_OUT / "mlp_5x256.expected.json").write_text(
        json.dumps({"input": [0.0] * 16, "expected": out_5, "tolerance": 2.0e-5}, indent=2)
        + "\n",
        encoding="utf-8",
    )

    # AC_003 — nontrivial normalizer (double-normalization would diverge).
    in_w, out_w = 6, 3
    mean = np.array([0.5, -1.0, 2.0, 0.25, -0.5, 1.5], dtype=np.float32)
    std = np.array([2.0, 0.5, 1.5, 3.0, 0.75, 1.25], dtype=np.float32)
    assert not np.allclose(mean, 0.0)
    assert not np.allclose(std, 1.0)
    model_norm, onnx_norm = _seeded_mlp(
        in_width=in_w,
        out_width=out_w,
        hidden=32,
        n_layers=3,
        seed=303,
        mean=mean,
        std=std,
    )
    obs = np.array([0.1, -0.2, 0.3, -0.4, 0.5, -0.6], dtype=np.float32)
    with torch.no_grad():
        expected = model_norm(torch.tensor(obs).unsqueeze(0)).numpy().reshape(-1).tolist()
        # Reference that would match a double-normalized C++ path (must differ).
        double = ((obs - mean) / (std + _NORM_EPS) - mean) / (std + _NORM_EPS)
        double_out = (
            model_norm.net(torch.tensor(double, dtype=torch.float32).unsqueeze(0))
            .numpy()
            .reshape(-1)
        )
        if np.max(np.abs(double_out - np.asarray(expected, dtype=np.float32))) < 1e-3:
            raise RuntimeError("AC_003 fixture would not catch double-normalization")
    _write_artifact(
        _OUT / "mlp_norm_nontrivial.uerlpol2", obs=in_w, act=out_w, onnx_bytes=onnx_norm
    )
    (_OUT / "mlp_norm_nontrivial.expected.json").write_text(
        json.dumps(
            {
                "input": obs.tolist(),
                "expected": expected,
                "tolerance": 2.0e-5,
                "mean": mean.tolist(),
                "std": std.tolist(),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    generate_phantomx_fixture()

    print("wrote fixtures to", _OUT)
    for name in sorted(p.name for p in _OUT.iterdir() if p.suffix in {".uerlpol2", ".json"}):
        print(" ", name)


if __name__ == "__main__":
    main()

"""Verify UERLPOL2 policy artifact container read/write and validation."""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import onnx
import pytest
from onnx import TensorProto, helper

from uerl.core.mdp.plan import PLAN_VERSION, ActionPlan, ObservationPlan, PlanOp
from uerl.errors import ConfigError
from uerl.policy.artifact import (
    ARTIFACT_FORMAT_VERSION,
    ArtifactMetadata,
    ArtifactTiming,
    PolicyArtifact,
    RobotRuntime,
    RobotRuntimeActuator,
)

_PARITY_DIR = Path(__file__).resolve().parents[2] / "parity" / "cases" / "artifact"
_CORPUS_PATH = _PARITY_DIR / "tiny_mlp.uerlpol2"


def _make_onnx(input_width: int, output_width: int) -> bytes:
    """Build a minimal real 2-layer MLP ONNX model (MatMul+Add twice)."""

    hidden = max(input_width, output_width)
    w1 = np.eye(input_width, hidden, dtype=np.float32)
    b1 = np.zeros((hidden,), dtype=np.float32)
    w2 = np.eye(hidden, output_width, dtype=np.float32)
    b2 = np.zeros((output_width,), dtype=np.float32)
    graph = helper.make_graph(
        [
            helper.make_node("MatMul", ["X", "W1"], ["H"]),
            helper.make_node("Add", ["H", "B1"], ["H1"]),
            helper.make_node("MatMul", ["H1", "W2"], ["Z"]),
            helper.make_node("Add", ["Z", "B2"], ["Y"]),
        ],
        "tiny_mlp",
        [helper.make_tensor_value_info("X", TensorProto.FLOAT, ["N", input_width])],
        [helper.make_tensor_value_info("Y", TensorProto.FLOAT, ["N", output_width])],
        [
            helper.make_tensor("W1", TensorProto.FLOAT, list(w1.shape), w1.flatten().tolist()),
            helper.make_tensor("B1", TensorProto.FLOAT, list(b1.shape), b1.tolist()),
            helper.make_tensor("W2", TensorProto.FLOAT, list(w2.shape), w2.flatten().tolist()),
            helper.make_tensor("B2", TensorProto.FLOAT, list(b2.shape), b2.tolist()),
        ],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    onnx.checker.check_model(model)
    return bytes(model.SerializeToString())


def _observation_plan(policy_width: int) -> ObservationPlan:
    return ObservationPlan(
        state_requirements=("robot.joint.left_c1.joint_position",),
        ops=(
            PlanOp(
                op="select",
                inputs=(),
                output="joint",
                width=policy_width,
                params={"field": "robot.joint.left_c1.joint_position"},
            ),
        ),
        groups={"policy": ("joint",)},
        group_widths={"policy": policy_width},
        plan_version=PLAN_VERSION,
    )


def _action_plan(policy_width: int) -> ActionPlan:
    return ActionPlan(
        ops=(
            PlanOp(op="policy_action", inputs=(), output="raw", width=policy_width),
            PlanOp(
                op="clip",
                inputs=("raw",),
                output="cmd",
                width=policy_width,
                params={"low": -1.0, "high": 1.0},
            ),
        ),
        command_fields=("cmd",),
        policy_width=policy_width,
        plan_version=PLAN_VERSION,
    )


def _robot_runtime(*, actuator_count: int = 2) -> RobotRuntime:
    return RobotRuntime(
        actuators=tuple(
            RobotRuntimeActuator(
                joint=f"joint_{index}",
                stiffness=25.0,
                damping=0.5,
                effort_limit=10.0,
                default_position=0.0,
            )
            for index in range(actuator_count)
        )
    )


def _metadata() -> ArtifactMetadata:
    return ArtifactMetadata(
        run_hash="a" * 64,
        git_identity={"commit": "deadbeef", "ref": "main", "dirty": False},
    )


def _make_artifact(
    *,
    obs_width: int = 4,
    act_width: int = 2,
    onnx_bytes: bytes | None = None,
) -> PolicyArtifact:
    return PolicyArtifact(
        format_version=ARTIFACT_FORMAT_VERSION,
        task_id="cartpole.balance",
        robot_id="cartpole",
        observation_plan=_observation_plan(obs_width),
        action_plan=_action_plan(act_width),
        robot_runtime=_robot_runtime(actuator_count=act_width),
        onnx=onnx_bytes if onnx_bytes is not None else _make_onnx(obs_width, act_width),
        metadata=_metadata(),
        timing=ArtifactTiming(1.0 / 60.0, 1, 1),
    )


def test_ac_py_unit_artifact_001_write_read_round_trip_is_bitwise_equal(tmp_path: Path) -> None:
    """AC_PY_UNIT_ARTIFACT_001: write→read preserves fields and ONNX bytes bitwise."""

    # Arrange
    original = _make_artifact()
    path = tmp_path / "policy.uerlpol2"
    second = tmp_path / "policy_rewritten.uerlpol2"

    # Act
    original.write(path)
    restored = PolicyArtifact.read(path)
    restored.write(second)

    # Assert — field equality including ONNX byte identity, not length alone.
    assert restored.format_version == original.format_version
    assert restored.task_id == original.task_id
    assert restored.robot_id == original.robot_id
    assert restored.observation_plan == original.observation_plan
    assert restored.action_plan == original.action_plan
    assert restored.robot_runtime == original.robot_runtime
    assert restored.timing == original.timing
    assert restored.metadata == original.metadata
    assert restored.onnx == original.onnx
    assert restored.onnx is not original.onnx
    assert path.read_bytes() == second.read_bytes()


def test_ac_py_unit_artifact_002_validate_detects_width_mismatches() -> None:
    """AC_PY_UNIT_ARTIFACT_002: validate catches obs and action width mismatches."""

    # Arrange — obs group width disagrees with ONNX input.
    obs_mismatch = _make_artifact(obs_width=4, act_width=2, onnx_bytes=_make_onnx(8, 2))
    # Arrange — action plan width disagrees with ONNX output.
    act_mismatch = _make_artifact(obs_width=4, act_width=2, onnx_bytes=_make_onnx(4, 3))

    # Act / Assert
    with pytest.raises(ConfigError) as obs_info:
        obs_mismatch.validate()
    assert obs_info.value.code == "ARTIFACT_OBS_WIDTH_MISMATCH"

    with pytest.raises(ConfigError) as act_info:
        act_mismatch.validate()
    assert act_info.value.code == "ARTIFACT_ACTION_WIDTH_MISMATCH"


def test_ac_py_unit_artifact_003_corrupt_containers_do_not_produce_instances(tmp_path: Path) -> None:
    """AC_PY_UNIT_ARTIFACT_003: four corruptions each raise a typed ConfigError."""

    # Arrange
    good = _make_artifact()
    good_path = tmp_path / "good.uerlpol2"
    good.write(good_path)
    good_bytes = good_path.read_bytes()

    cases: list[tuple[str, bytes, str]] = []

    bad_magic = bytearray(good_bytes)
    bad_magic[0:8] = b"BADMAGIC"
    cases.append(("bad_magic", bytes(bad_magic), "ARTIFACT_BAD_MAGIC"))

    bad_version = bytearray(good_bytes)
    struct.pack_into("<I", bad_version, 8, ARTIFACT_FORMAT_VERSION + 1)
    cases.append(("bad_version", bytes(bad_version), "ARTIFACT_VERSION_MISMATCH"))

    # Claim a huge onnx_length that overruns the file.
    json_length = struct.unpack_from("<I", good_bytes, 12)[0]
    onnx_len_offset = 16 + json_length
    oob = bytearray(good_bytes)
    struct.pack_into("<Q", oob, onnx_len_offset, len(good_bytes) + 1000)
    cases.append(("onnx_oob", bytes(oob), "ARTIFACT_ONNX_LENGTH_OOB"))

    # Truncate mid-JSON.
    truncated = good_bytes[: 16 + max(1, json_length // 2)]
    cases.append(("json_truncated", truncated, "ARTIFACT_JSON_TRUNCATED"))

    # Act / Assert
    for label, payload, code in cases:
        path = tmp_path / f"{label}.uerlpol2"
        path.write_bytes(payload)
        with pytest.raises(ConfigError) as exc_info:
            PolicyArtifact.read(path)
        assert exc_info.value.code == code, label


def test_ac_py_unit_artifact_004_rejects_non_onnx_bytes(tmp_path: Path) -> None:
    """AC_PY_UNIT_ARTIFACT_004: arbitrary ONNX-segment bytes are not accepted."""

    # Arrange — build a structurally valid container whose ONNX segment is garbage.
    artifact = _make_artifact()
    path = tmp_path / "fake_onnx.uerlpol2"
    artifact.write(path)
    data = bytearray(path.read_bytes())
    json_length = struct.unpack_from("<I", data, 12)[0]
    onnx_len_offset = 16 + json_length
    onnx_start = onnx_len_offset + 8
    fake = b"not-a-real-onnx-model" + b"\x00" * 32
    struct.pack_into("<Q", data, onnx_len_offset, len(fake))
    data = data[:onnx_start] + fake
    path.write_bytes(bytes(data))

    # Act / Assert
    with pytest.raises(ConfigError) as exc_info:
        PolicyArtifact.read(path)
    assert exc_info.value.code == "ARTIFACT_ONNX_INVALID"


def test_artifact_has_no_architecture_or_weight_fields() -> None:
    """Artifact JSON payload excludes topology / weight / normalizer fields."""

    artifact = _make_artifact()
    payload = artifact._json_payload()  # noqa: SLF001 — intentional contract check
    assert "architecture" not in payload
    assert "layers" not in payload
    assert "weights" not in payload
    assert "activation" not in payload
    assert "mean" not in payload
    assert "std" not in payload
    assert "epsilon" not in payload
    assert set(payload) == {
        "task_id",
        "robot_id",
        "observation_plan",
        "action_plan",
        "robot_runtime",
        "timing",
        "metadata",
    }
    assert set(payload["robot_runtime"]) == {"actuators"}
    assert set(payload["robot_runtime"]["actuators"][0]) == {
        "joint",
        "stiffness",
        "damping",
        "effort_limit",
        "default_position",
    }
    assert set(payload["metadata"]) == {"run_hash", "git_identity"}


def test_matching_artifact_passes_validate() -> None:
    """Happy-path validate succeeds when plan widths match ONNX shapes."""

    artifact = _make_artifact(obs_width=4, act_width=2)
    artifact.validate()


def test_artifact_timing_rejects_missing_or_invalid_values() -> None:
    """Timing is required on the wire and validates both derived intervals."""

    artifact = _make_artifact()
    payload = artifact._json_payload()  # noqa: SLF001 — intentional contract check
    payload.pop("timing")
    with pytest.raises(ConfigError) as missing:
        PolicyArtifact.from_bytes(_rewrite_json_payload(artifact, payload))
    assert missing.value.code == "ARTIFACT_MISSING_FIELD"

    with pytest.raises(ConfigError) as invalid:
        ArtifactTiming(0.005, 7, 1)
    assert invalid.value.code == "ARTIFACT_TIMING_INVALID"


@pytest.mark.parametrize(
    "values",
    [
        (float("nan"), 1, 1),
        (0.0, 1, 1),
        (0.005, 0, 1),
        (0.005, 1, 2**31),
    ],
)
def test_artifact_timing_rejects_non_finite_or_bad_endpoints(values: tuple[float, int, int]) -> None:
    """Timing rejects non-finite dt and endpoints outside the positive int32 range."""

    with pytest.raises(ConfigError, match="timing"):
        ArtifactTiming(*values)


def _rewrite_json_payload(artifact: PolicyArtifact, payload: dict[str, object]) -> bytes:
    """Build a structurally valid container with a deliberately changed JSON root."""

    import struct

    from uerl.core.config.canonical import canonical_json

    json_bytes = canonical_json(payload).encode("utf-8")
    return (
        struct.pack("<8sII", b"UERLPOL2", ARTIFACT_FORMAT_VERSION, len(json_bytes))
        + json_bytes
        + struct.pack("<Q", len(artifact.onnx))
        + artifact.onnx
    )


def test_parity_corpus_artifact_round_trips() -> None:
    """Committed parity corpus is a real ONNX-backed artifact Python can read."""

    assert _CORPUS_PATH.is_file(), f"missing artifact corpus {_CORPUS_PATH}"
    artifact = PolicyArtifact.read(_CORPUS_PATH)
    assert artifact.task_id == "cartpole.balance"
    assert artifact.robot_id == "cartpole"
    assert [op.op for op in artifact.observation_plan.ops] == ["select"]
    assert artifact.observation_plan.group_widths["policy"] == 4
    assert artifact.action_plan.policy_width == 2
    # Prove ONNX is a real model (not placeholder bytes).
    model = onnx.load_model_from_string(artifact.onnx)
    onnx.checker.check_model(model)
    assert model.graph.input[0].type.tensor_type.shape.dim[-1].dim_value == 4
    assert model.graph.output[0].type.tensor_type.shape.dim[-1].dim_value == 2
    artifact.validate()

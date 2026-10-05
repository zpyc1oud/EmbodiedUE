"""Verify policy export binds DirectTask plans with RSL-RL ONNX into UERLPOL2."""

from __future__ import annotations

from pathlib import Path
from typing import cast

import numpy as np
import onnx
import onnxruntime as ort
import pytest
import torch
from onnx import TensorProto, helper
from rsl_rl.models.mlp_model import MLPModel
from tensordict import TensorDict

from uerl.core.direct.task import DirectTask
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
from uerl.training.export import export_policy


def _make_rsl_deterministic_export(
    tmp_path: Path,
) -> tuple[ort.InferenceSession, np.ndarray, np.ndarray, float, torch.nn.Module]:
    """Export a real RSL-RL MLP wrapper with normalization and a non-zero Gaussian std."""

    torch.manual_seed(20260911)
    model = MLPModel(
        obs=TensorDict({"policy": torch.zeros((1, 4), dtype=torch.float32)}, batch_size=[1]),
        obs_groups={"actor": ["policy"]},
        obs_set="actor",
        output_dim=2,
        hidden_dims=(8, 8),
        activation="elu",
        obs_normalization=True,
        distribution_cfg={
            "class_name": "rsl_rl.modules.distribution:GaussianDistribution",
            "init_std": 0.5,
            "learn_std": False,
        },
    )
    model.eval()
    with torch.no_grad():
        model.obs_normalizer._mean.copy_(torch.tensor([[0.25, -0.5, 1.0, 0.75]]))
        model.obs_normalizer._std.copy_(torch.tensor([[0.5, 1.5, 2.0, 0.75]]))

    observation = np.asarray([[0.75, -1.25, 2.5, -0.25]], dtype=np.float32)
    observation_tensor = torch.from_numpy(observation)
    reference = model(
        TensorDict({"policy": observation_tensor}, batch_size=[1]),
        stochastic_output=False,
    ).detach().cpu().numpy()

    # The model has a non-zero exploration std, but the export wrapper must use
    # the deterministic output transform and keep normalization inside the graph.
    model(
        TensorDict({"policy": observation_tensor}, batch_size=[1]),
        stochastic_output=True,
    )
    std = float(model.output_std.detach().cpu().reshape(-1)[0])
    onnx_model = model.as_onnx(verbose=False)
    onnx_model.eval()
    onnx_path = tmp_path / "rsl_deterministic.onnx"
    torch.onnx.export(
        onnx_model,
        (observation_tensor,),
        str(onnx_path),
        export_params=True,
        opset_version=18,
        input_names=["obs"],
        output_names=["actions"],
    )
    session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    return session, observation, reference, std, onnx_model


def test_ac_py_unit_export_006_rsl_onnx_is_bitwise_stable_for_nonzero_observation(
    tmp_path: Path,
) -> None:
    """AC_PY_UNIT_EXPORT_006: repeated ONNX inference is byte-identical."""

    session, observation, reference, std, _ = _make_rsl_deterministic_export(tmp_path)
    outputs = [session.run(["actions"], {"obs": observation})[0] for _ in range(3)]

    assert np.any(observation != 0.0)
    assert std == pytest.approx(0.5, abs=1.0e-7)
    assert outputs[0].tobytes() == outputs[1].tobytes() == outputs[2].tobytes()
    np.testing.assert_allclose(outputs[0], reference, atol=2.0e-5, rtol=2.0e-5)


def test_ac_py_unit_export_007_deterministic_reference_rejects_fixed_noise(tmp_path: Path) -> None:
    """AC_PY_UNIT_EXPORT_007: ONNX matches PyTorch, while injected noise does not."""

    session, observation, reference, _, onnx_model = _make_rsl_deterministic_export(tmp_path)
    output = session.run(["actions"], {"obs": observation})[0]
    np.testing.assert_allclose(output, reference, atol=2.0e-5, rtol=2.0e-5)

    class _FixedNoiseExport(torch.nn.Module):
        def __init__(self, base: torch.nn.Module) -> None:
            super().__init__()
            self.base = base

        def forward(self, values: torch.Tensor) -> torch.Tensor:
            return cast(torch.Tensor, self.base(values)) + 0.125

    # A fixed non-zero perturbation is a concrete wrong export; it must not be
    # accepted by the same deterministic reference tolerance.
    wrong_path = tmp_path / "rsl_wrong_noise.onnx"
    wrong_model = _FixedNoiseExport(onnx_model).eval()
    observation_tensor = torch.from_numpy(observation)
    torch.onnx.export(
        wrong_model,
        (observation_tensor,),
        str(wrong_path),
        export_params=True,
        opset_version=18,
        input_names=["obs"],
        output_names=["actions"],
    )
    wrong_session = ort.InferenceSession(str(wrong_path), providers=["CPUExecutionProvider"])
    wrong_output = wrong_session.run(["actions"], {"obs": observation})[0]
    assert not np.allclose(wrong_output, reference, atol=2.0e-5, rtol=2.0e-5)


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


def _observation_plan(policy_width: int, *, sentinel_output: str = "joint") -> ObservationPlan:
    return ObservationPlan(
        state_requirements=("robot.joint.left_c1.joint_position",),
        ops=(
            PlanOp(
                op="select",
                inputs=(),
                output=sentinel_output,
                width=policy_width,
                params={"field": "robot.joint.left_c1.joint_position"},
            ),
        ),
        groups={"policy": (sentinel_output,)},
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


def _metadata() -> ArtifactMetadata:
    return ArtifactMetadata(
        run_hash="a" * 64,
        git_identity={"commit": "deadbeef", "ref": "main", "dirty": False},
    )


def _timing() -> ArtifactTiming:
    return ArtifactTiming(1.0 / 60.0, 1, 1)


def _robot_runtime(*, actuator_count: int) -> RobotRuntime:
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


class _StaticDirectTask(DirectTask):
    """DirectTask test double holding the two plans used by export."""

    def __init__(self, observation_plan: ObservationPlan, action_plan: ActionPlan) -> None:
        super().__init__()
        self._observation_plan = observation_plan
        self._action_plan = action_plan

    @property
    def observation_plan(self) -> ObservationPlan:
        return self._observation_plan

    @property
    def action_plan(self) -> ActionPlan:
        return self._action_plan


class _CountingDirectTask(DirectTask):
    """DirectTask that records how many times each plan property is read.

    Distinguishes "take the held object" (one access, JSON matches) from
    "rebuild from config" (would not touch these properties, or would lose a
    sentinel slot name).
    """

    def __init__(self, observation_plan: ObservationPlan, action_plan: ActionPlan) -> None:
        super().__init__()
        self._observation_plan = observation_plan
        self._action_plan = action_plan
        self.observation_plan_access_count = 0
        self.action_plan_access_count = 0

    @property
    def observation_plan(self) -> ObservationPlan:
        self.observation_plan_access_count += 1
        return self._observation_plan

    @property
    def action_plan(self) -> ActionPlan:
        self.action_plan_access_count += 1
        return self._action_plan


class _FakeOnnxRunner:
    """Test-only runner: writes a fixed ONNX file via export_policy_to_onnx."""

    def __init__(self, onnx_bytes: bytes, *, fail: bool = False) -> None:
        self._onnx_bytes = onnx_bytes
        self._fail = fail
        self.export_calls = 0
        self.last_export_path: Path | None = None

    def export_policy_to_onnx(
        self,
        path: str,
        filename: str = "policy.onnx",
        verbose: bool = False,
    ) -> None:
        del verbose
        self.export_calls += 1
        target_dir = Path(path)
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / filename
        target.write_bytes(self._onnx_bytes)
        self.last_export_path = target
        if self._fail:
            raise RuntimeError("forced export failure")


def test_ac_py_unit_export_001_artifact_read_validate_matches_plan_widths(tmp_path: Path) -> None:
    """AC_PY_UNIT_EXPORT_001: exported artifact is readable/valid; widths match ONNX."""

    # Arrange
    obs_width, act_width = 4, 2
    onnx_bytes = _make_onnx(obs_width, act_width)
    task = _StaticDirectTask(
        observation_plan=_observation_plan(obs_width),
        action_plan=_action_plan(act_width),
    )
    runner = _FakeOnnxRunner(onnx_bytes)
    output = tmp_path / "policy.uerlpol2"

    # Act
    written = export_policy(
        task=task,
        runner=runner,
        output=output,
        metadata=_metadata(),
        robot_runtime=_robot_runtime(actuator_count=2),
        timing=_timing(),
        task_id="cartpole.balance",
        robot_id="cartpole",
    )
    restored = PolicyArtifact.read(output)
    restored.validate()

    # Assert
    assert written.format_version == ARTIFACT_FORMAT_VERSION
    assert restored.onnx == onnx_bytes
    assert restored.timing == _timing()
    assert restored.observation_plan.group_widths["policy"] == obs_width
    assert restored.action_plan.policy_width == act_width
    assert runner.export_calls == 1


def test_ac_py_unit_export_002_takes_direct_task_objects_not_rebuild(tmp_path: Path) -> None:
    """AC_PY_UNIT_EXPORT_002: exporter reads DirectTask plans (access count + sentinel).

    Assertion strategy (ticket choice): counting property access proves the
    exporter consulted the DirectTask; a sentinel slot name proves it did not
    rebuild from a config that never contained that name. Content equality alone
    would not distinguish "same object" from "rebuild from same config".
    """

    # Arrange — sentinel slot name would be lost if the exporter rebuilt plans.
    sentinel = "sentinel_export_slot"
    plan = _observation_plan(4, sentinel_output=sentinel)
    action = _action_plan(2)
    source = _CountingDirectTask(plan, action)
    runner = _FakeOnnxRunner(_make_onnx(4, 2))
    output = tmp_path / "policy.uerlpol2"

    # Act
    artifact = export_policy(
        task=source,
        runner=runner,
        output=output,
        metadata=_metadata(),
        robot_runtime=_robot_runtime(actuator_count=2),
        timing=_timing(),
        task_id="cartpole.balance",
        robot_id="cartpole",
    )

    # Assert — DirectTask plans were read (not ignored / not rebuilt from elsewhere).
    assert source.observation_plan_access_count == 1
    assert source.action_plan_access_count == 1
    assert artifact.observation_plan.to_json() == plan.to_json()
    assert artifact.action_plan.to_json() == action.to_json()
    assert artifact.observation_plan.ops[0].output == sentinel
    assert "policy" in artifact.observation_plan.groups
    assert artifact.observation_plan.groups["policy"] == (sentinel,)


def test_ac_py_unit_export_003_same_checkpoint_yields_identical_artifact_bytes(
    tmp_path: Path,
) -> None:
    """AC_PY_UNIT_EXPORT_003: identical inputs → bitwise-identical artifact bytes.

    ArtifactMetadata has no timestamps; fixed metadata + fixed ONNX bytes make
    byte identity the primary criterion (no degradation to inference-output
    comparison needed for this fake-runner path).
    """

    # Arrange
    onnx_bytes = _make_onnx(4, 2)
    task = _StaticDirectTask(
        observation_plan=_observation_plan(4),
        action_plan=_action_plan(2),
    )
    metadata = _metadata()
    first_out = tmp_path / "a.uerlpol2"
    second_out = tmp_path / "b.uerlpol2"

    # Act
    export_policy(
        task=task,
        runner=_FakeOnnxRunner(onnx_bytes),
        output=first_out,
        metadata=metadata,
        robot_runtime=_robot_runtime(actuator_count=2),
        timing=_timing(),
        task_id="cartpole.balance",
        robot_id="cartpole",
    )
    export_policy(
        task=task,
        runner=_FakeOnnxRunner(onnx_bytes),
        output=second_out,
        metadata=metadata,
        robot_runtime=_robot_runtime(actuator_count=2),
        timing=_timing(),
        task_id="cartpole.balance",
        robot_id="cartpole",
    )

    # Assert
    assert first_out.read_bytes() == second_out.read_bytes()


def test_ac_py_unit_export_004_onnx_segment_matches_runner_file_bytes(tmp_path: Path) -> None:
    """AC_PY_UNIT_EXPORT_004: artifact ONNX bytes == runner.export_policy_to_onnx output.

    Proves the exporter did not re-serialize, quantize, or otherwise transform
    weights between RSL-RL's file and the artifact segment.
    """

    # Arrange
    onnx_bytes = _make_onnx(4, 2)
    runner = _FakeOnnxRunner(onnx_bytes)
    task = _StaticDirectTask(
        observation_plan=_observation_plan(4),
        action_plan=_action_plan(2),
    )
    output = tmp_path / "policy.uerlpol2"

    # Act
    artifact = export_policy(
        task=task,
        runner=runner,
        output=output,
        metadata=_metadata(),
        robot_runtime=_robot_runtime(actuator_count=2),
        timing=_timing(),
        task_id="cartpole.balance",
        robot_id="cartpole",
    )

    # Assert — byte identity with what the runner wrote (captured before cleanup).
    assert artifact.onnx == onnx_bytes
    restored = PolicyArtifact.read(output)
    assert restored.onnx == onnx_bytes


def test_ac_py_unit_export_005_temp_files_cleaned_on_success_and_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """AC_PY_UNIT_EXPORT_005: temporary export directory is removed on both paths."""

    import tempfile

    created: list[Path] = []
    real_mkdtemp = tempfile.mkdtemp

    def tracking_mkdtemp(
        suffix: str | None = None,
        prefix: str | None = None,
        dir: str | None = None,
    ) -> str:
        path = Path(real_mkdtemp(suffix=suffix, prefix=prefix, dir=dir))
        created.append(path)
        return str(path)

    monkeypatch.setattr(tempfile, "mkdtemp", tracking_mkdtemp)

    task = _StaticDirectTask(
        observation_plan=_observation_plan(4),
        action_plan=_action_plan(2),
    )
    onnx_bytes = _make_onnx(4, 2)
    success_out = tmp_path / "ok.uerlpol2"
    fail_out = tmp_path / "fail.uerlpol2"

    # Act — success path
    export_policy(
        task=task,
        runner=_FakeOnnxRunner(onnx_bytes),
        output=success_out,
        metadata=_metadata(),
        robot_runtime=_robot_runtime(actuator_count=2),
        timing=_timing(),
        task_id="cartpole.balance",
        robot_id="cartpole",
    )
    # Act — failure path (runner writes then raises)
    with pytest.raises(RuntimeError, match="forced export failure"):
        export_policy(
            task=task,
            runner=_FakeOnnxRunner(onnx_bytes, fail=True),
            output=fail_out,
            metadata=_metadata(),
            robot_runtime=_robot_runtime(actuator_count=2),
            timing=_timing(),
            task_id="cartpole.balance",
            robot_id="cartpole",
        )

    # Assert
    assert len(created) == 2
    for path in created:
        assert not path.exists(), f"temp export dir left behind: {path}"
    assert success_out.is_file()
    assert not fail_out.exists()


def test_export_capability_rejects_python_only_task_before_onnx_export(tmp_path: Path) -> None:
    """Unsupported Python task math must fail before the expensive exporter runs."""

    task = DirectTask()
    runner = _FakeOnnxRunner(b"not reached")

    with pytest.raises(ConfigError) as raised:
        export_policy(
            task=task,
            runner=runner,
            output=tmp_path / "unsupported.uerlpol2",
            metadata=_metadata(),
            robot_runtime=_robot_runtime(actuator_count=1),
            timing=_timing(),
            task_id="python.only",
            robot_id="test.robot",
        )

    assert raised.value.code == "TASK_CAPABILITY_UNSUPPORTED"
    assert raised.value.path == "task.capabilities.export"
    assert "Manager-generated" in str(raised.value)
    assert runner.export_calls == 0
    assert not (tmp_path / "unsupported.uerlpol2").exists()

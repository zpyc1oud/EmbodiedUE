"""Bind RSL-RL ONNX export output with a DirectTask into a UERLPOL2 artifact.

Uses ``OnPolicyRunner.export_policy_to_onnx`` directly. Does not traverse weights,
describe topology, or apply observation normalization (normalization is already
embedded in the exported ONNX graph).

The DirectTask owns the observation/action plans used by both training and
deployment, so export reads those plans directly from the bound task.
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Protocol

from uerl.core.config.models import ResolvedRunConfig
from uerl.core.direct.task import DirectTask
from uerl.errors import ConfigError
from uerl.policy.artifact import (
    ARTIFACT_FORMAT_VERSION,
    ArtifactMetadata,
    ArtifactTiming,
    PolicyArtifact,
    RobotRuntime,
    RobotRuntimeActuator,
)

# Default ONNX filename written by RSL-RL ``export_policy_to_onnx``.
_DEFAULT_ONNX_FILENAME = "policy.onnx"


def robot_runtime_from_config(config: ResolvedRunConfig) -> RobotRuntime:
    """Derive the deployment actuator segment from resolved Robot semantics."""

    semantics = config.worker.robot_semantics
    if semantics is None:
        raise ConfigError(
            "resolved Task has no Robot semantics for artifact export",
            code="MISSING_ROBOT_SEMANTICS",
            path="worker.robot_semantics",
        )
    return RobotRuntime(
        actuators=tuple(
            RobotRuntimeActuator(
                joint=actuator.joint,
                stiffness=actuator.stiffness,
                damping=actuator.damping,
                effort_limit=actuator.effort_limit,
                default_position=actuator.default_pos,
            )
            for actuator in semantics.actuators
        )
    )


class _OnnxExporter(Protocol):
    """Minimal surface required from RSL-RL ``OnPolicyRunner`` (or a test double)."""

    def export_policy_to_onnx(
        self,
        path: str,
        filename: str = "policy.onnx",
        verbose: bool = False,
    ) -> None: ...


def export_policy(
    *,
    task: DirectTask,
    runner: _OnnxExporter,
    output: Path,
    metadata: ArtifactMetadata,
    robot_runtime: RobotRuntime,
    timing: ArtifactTiming,
    task_id: str,
    robot_id: str,
) -> PolicyArtifact:
    """Bind plans from a DirectTask with RSL-RL ONNX bytes into a UERLPOL2 artifact.

    Key invariant: ``observation_plan`` / ``action_plan`` are taken as the
    **objects** held by ``task`` — not rebuilt from config. That object identity
    is the root of train/deploy consistency.

    Flow:
      1. ``runner.export_policy_to_onnx(tmp_dir)`` → ``policy.onnx`` (caller loads the checkpoint)
      2. read ONNX bytes
      3. write ``PolicyArtifact`` to ``output``
      4. clean up the temporary directory (success and failure)

    ArtifactMetadata has no timestamp fields — determinism is the default when
    the caller passes fixed ``metadata`` (same checkpoint → identical artifact
    bytes). No ``include_timestamps`` flag is needed.

    Export path: RSL-RL ``export_policy_to_onnx`` (ONNX). TorchScript fallback is
    not used; if ONNX export fails in-environment, document the failure and switch
    the artifact segment — never hand-roll weight serialization.
    """

    observation_plan = task.observation_plan
    action_plan = task.action_plan

    with tempfile.TemporaryDirectory(prefix="uerl-export-") as temporary_dir:
        temporary_root = Path(temporary_dir)
        runner.export_policy_to_onnx(str(temporary_root), filename=_DEFAULT_ONNX_FILENAME)
        onnx_path = temporary_root / _DEFAULT_ONNX_FILENAME
        if not onnx_path.is_file():
            raise FileNotFoundError(
                f"RSL-RL export_policy_to_onnx did not write {_DEFAULT_ONNX_FILENAME!r} "
                f"under {temporary_root}"
            )
        # RSL-RL may emit external weight files; the artifact stores one self-contained
        # ONNX byte string, so inline any external tensors before packaging.
        import onnx
        from onnx.external_data_helper import convert_model_from_external_data

        model = onnx.load(str(onnx_path), load_external_data=True)
        convert_model_from_external_data(model)
        onnx_bytes = model.SerializeToString()

        artifact = PolicyArtifact(
            format_version=ARTIFACT_FORMAT_VERSION,
            task_id=task_id,
            robot_id=robot_id,
            observation_plan=observation_plan,
            action_plan=action_plan,
            robot_runtime=robot_runtime,
            onnx=onnx_bytes,
            metadata=metadata,
            timing=timing,
        )
        artifact.validate()
        artifact.write(Path(output))
        return artifact


__all__ = [
    "export_policy",
    "robot_runtime_from_config",
]

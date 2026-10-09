"""Check effective configuration before any UE process exists."""

from dataclasses import replace
from pathlib import Path

import pytest
import yaml

from tests.python.unit.test_policy_artifact import _make_artifact
from uerl.cli.check import _task_report, main
from uerl.core.config.snapshot import resolved_config_to_yaml
from uerl.errors import ConfigError
from uerl.policy.artifact import ArtifactTiming
from uerl.training import build_run_config, robot_runtime_from_config

TASK = "UERL-PhantomX-Walk-v0"


def test_override_report_contains_actual_timing_and_actuators() -> None:
    report, _ = _task_report(TASK, overrides={"worker.physics_dt": "0.005", "worker.decimation": "[2, 6]"})
    assert report["control_interval_s"] == [0.01, 0.03]
    assert report["actuator_count"] == 18
    assert len(report["actuator_joints"]) == 18  # type: ignore[arg-type]
    assert report["pending_host_checks"]


def test_invalid_override_fails_with_field_path(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["task", TASK, "--worker.physics_dt", "-1"]) == 1
    assert "worker.physics_dt" in capsys.readouterr().out


def test_yaml_report_does_not_claim_native_binding(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["task", TASK, "--yaml"]) == 0
    report = yaml.safe_load(capsys.readouterr().out)
    assert "native joint/body binding" in report["pending_host_checks"]
    assert report["artifact"] is None


def test_saved_run_uses_recorded_timing_and_refuses_override(tmp_path: Path) -> None:
    config = build_run_config(TASK, overrides={"worker.physics_dt": ".006", "worker.decimation": "[3, 3]"})
    (tmp_path / "resolved_config.yaml").write_text(resolved_config_to_yaml(config))
    report, _ = _task_report(TASK, run=tmp_path)
    assert report["control_interval_s"] == pytest.approx([0.018, 0.018])
    with pytest.raises(ConfigError, match="do not accept overrides"):
        _task_report(TASK, run=tmp_path, overrides={"worker.physics_dt": ".005"})


def test_artifact_matching_and_timing_mismatch_use_real_file(tmp_path: Path) -> None:
    config = build_run_config(TASK)
    artifact = replace(
        _make_artifact(act_width=18),
        task_id=TASK,
        robot_id=config.worker.robot_id,
        robot_runtime=robot_runtime_from_config(config),
        timing=ArtifactTiming(config.worker.physics_dt, *config.worker.decimation),
    )
    path = tmp_path / "policy.uerlpol2"
    artifact.write(path)
    report, _ = _task_report(TASK, artifact_path=path)
    assert report["artifact"] is not None
    replace(artifact, timing=ArtifactTiming(0.01, 1, 1)).write(path)
    with pytest.raises(ConfigError) as error:
        _task_report(TASK, artifact_path=path)
    assert error.value.path == "artifact.timing"


@pytest.mark.parametrize("field", ["task_id", "robot_id", "robot_runtime"])
def test_artifact_identity_or_actuator_mismatch_names_field(tmp_path: Path, field: str) -> None:
    config = build_run_config(TASK)
    artifact = replace(
        _make_artifact(act_width=18),
        task_id=TASK,
        robot_id=config.worker.robot_id,
        robot_runtime=robot_runtime_from_config(config),
        timing=ArtifactTiming(config.worker.physics_dt, *config.worker.decimation),
    )
    if field == "task_id":
        artifact = replace(artifact, task_id="other.task")
    elif field == "robot_id":
        artifact = replace(artifact, robot_id="other.robot")
    else:
        first, *rest = artifact.robot_runtime.actuators
        runtime = replace(artifact.robot_runtime, actuators=(replace(first, damping=first.damping + 1), *rest))
        artifact = replace(artifact, robot_runtime=runtime)
    path = tmp_path / "mismatch.uerlpol2"
    artifact.write(path)
    with pytest.raises(ConfigError) as error:
        _task_report(TASK, artifact_path=path)
    assert error.value.path == f"artifact.{field}"

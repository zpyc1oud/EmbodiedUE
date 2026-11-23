"""Check export capability preflight order without launching Unreal Engine."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from types import SimpleNamespace

import pytest

from uerl import DirectTask
from uerl.application.run_config import RunConfig
from uerl.cli import export as export_cli
from uerl.core.config import ResolvedRunConfig
from uerl.core.direct import env as direct_env_module
from uerl.core.direct.capabilities import CapabilityStatus, TaskCapabilities, TaskCapability
from uerl.runtime import session as session_module
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config
from uerl.training import rsl_rl as rsl_rl_module


class _UnknownExportTask(DirectTask):
    """Represent a Manager Task that remains unproven after RobotSpec binding."""

    def __init__(self, events: list[str]) -> None:
        super().__init__()
        self.events = events

    @property
    def capabilities(self) -> TaskCapabilities:
        return TaskCapabilities(
            train=TaskCapability(CapabilityStatus.SUPPORTED),
            evaluate=TaskCapability(CapabilityStatus.SUPPORTED),
            export=TaskCapability(CapabilityStatus.UNKNOWN, "Manager export equivalence is not established"),
        )


class _FakeRegistry:
    def __init__(self, task: DirectTask) -> None:
        self.task = task

    def resolve(self, task_id: str) -> SimpleNamespace:
        return SimpleNamespace(task_id=task_id, robot_id="uerl.robot.skeletal_mesh")

    def create_task(self, task_id: str, config: object) -> DirectTask:
        del task_id, config
        return self.task

    def create_curriculum(self, *args: object, **kwargs: object) -> None:
        del args, kwargs
        return None


class _RunConfigSource:
    def __init__(self, config: ResolvedRunConfig) -> None:
        self.config = config

    def resolve(self, overrides: Mapping[str, str]) -> RunConfig:
        del overrides
        return RunConfig(self.config, self.config.normalized_hash, False, "test Run config")


def _prepare_export_cli(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    task: DirectTask,
) -> tuple[Path, Path, list[str]]:
    events: list[str] = []
    checkpoint = tmp_path / "model_final.pt"
    checkpoint.write_bytes(b"checkpoint placeholder")
    output = tmp_path / "policy.uerlpol2"
    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={"session.mode": "attach", "worker.slot_count": "1"},
    )
    source = _RunConfigSource(config)
    registry = _FakeRegistry(task)

    monkeypatch.setattr(export_cli, "find_run_directory_for_checkpoint", lambda _: None)
    monkeypatch.setattr(export_cli, "load_run_config_source", lambda *args, **kwargs: source)
    monkeypatch.setattr("uerl.tasks.registry.create_default_registry", lambda: registry)
    return checkpoint, output, events


def _invoke_export(checkpoint: Path, output: Path) -> int:
    return export_cli.main(
        [
            "--checkpoint",
            str(checkpoint),
            "--output",
            str(output),
            "--session.mode",
            "attach",
        ]
    )


def test_known_unsupported_export_fails_before_session_open(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    checkpoint, output, events = _prepare_export_cli(monkeypatch, tmp_path, DirectTask())

    def unexpected_open(
        cls: type[session_module.UERLSession], /, *args: object, **kwargs: object
    ) -> None:
        del cls, args, kwargs
        events.append("session.open")
        pytest.fail("known unsupported export reached UERLSession.open")

    monkeypatch.setattr(session_module.UERLSession, "open", classmethod(unexpected_open))

    assert _invoke_export(checkpoint, output) == 1

    output_text = capsys.readouterr().out
    assert "[FAIL] config --task.capabilities.export" in output_text
    assert "no Manager-generated" in output_text
    assert events == []
    assert not output.exists()


def test_unknown_export_binds_then_fails_before_runner_or_checkpoint_load(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    events: list[str] = []
    task = _UnknownExportTask(events)
    checkpoint, output, _ = _prepare_export_cli(monkeypatch, tmp_path, task)

    class FakeSession:
        def close(self, reason: str = "session_close") -> None:
            del reason
            events.append("session.close")

    def open_session(
        cls: type[session_module.UERLSession],
        /,
        config: object,
        **kwargs: object,
    ) -> FakeSession:
        del cls, config, kwargs
        events.append("session.open")
        return FakeSession()

    class FakeDirectEnv:
        def __init__(self, session: object, bound_task: _UnknownExportTask, **kwargs: object) -> None:
            del session, kwargs
            events.append("direct_env.initial_reset_complete")
            bound_task.events.append("task.bound")

        def close(self, reason: str) -> None:
            del reason
            events.append("direct_env.close")

    def unexpected_runner(*args: object, **kwargs: object) -> None:
        del args, kwargs
        pytest.fail("unknown export reached vector runner construction or checkpoint load")

    monkeypatch.setattr(session_module.UERLSession, "open", classmethod(open_session))
    monkeypatch.setattr(session_module, "UERLSessionAdapter", lambda session, device: session)
    monkeypatch.setattr(direct_env_module, "UERLDirectEnv", FakeDirectEnv)
    monkeypatch.setattr(rsl_rl_module, "UERLVecEnvWrapper", unexpected_runner)
    monkeypatch.setattr(rsl_rl_module, "UERLOnPolicyRunner", unexpected_runner)

    assert _invoke_export(checkpoint, output) == 1

    output_text = capsys.readouterr().out
    assert "[FAIL] config --task.capabilities.export" in output_text
    assert "Manager export equivalence is not established" in output_text
    assert events == [
        "session.open",
        "direct_env.initial_reset_complete",
        "task.bound",
        "direct_env.close",
    ]
    assert not output.exists()


def _prepare_saved_run_with_contract(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    *,
    contract_task_id: str | None = CARTPOLE_TASK_ID,
) -> tuple[Path, Path, Path]:
    from uerl.core.mdp.plan import PLAN_VERSION, ActionPlan, ObservationPlan, PlanOp
    from uerl.policy.artifact import RobotRuntime, RobotRuntimeActuator
    from uerl.training.export_contract import EXPORT_CONTRACT_FILENAME, ExportContract, write_export_contract

    run_dir = tmp_path / "run"
    run_dir.mkdir(parents=True)
    checkpoint = run_dir / "model_final.pt"
    checkpoint.write_bytes(b"checkpoint placeholder")
    output = run_dir / "exported" / "cartpole.uerlpol2"
    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={
            "worker.slot_count": "1",
            "logging.run_directory": str(run_dir),
        },
    )
    contract = ExportContract(
        task_id=contract_task_id or CARTPOLE_TASK_ID,
        robot_id=config.worker.robot_id,
        observation_plan=ObservationPlan(
            state_requirements=("robot.joint.cart.joint_position",),
            ops=(
                PlanOp(
                    op="select",
                    inputs=(),
                    output="joint",
                    width=4,
                    params={"field": "robot.joint.cart.joint_position"},
                ),
            ),
            groups={"policy": ("joint",)},
            group_widths={"policy": 4},
            plan_version=PLAN_VERSION,
        ),
        action_plan=ActionPlan(
            ops=(
                PlanOp(op="policy_action", inputs=(), output="raw", width=1),
                PlanOp(op="clip", inputs=("raw",), output="cmd", width=1, params={"low": -1.0, "high": 1.0}),
            ),
            command_fields=("cmd",),
            policy_width=1,
            plan_version=PLAN_VERSION,
        ),
        input_requirements=(),
        observation_group_widths={"policy": 4},
        num_actions=1,
    )
    write_export_contract(run_dir / EXPORT_CONTRACT_FILENAME, contract)
    source = _RunConfigSource(config)

    class _CartPoleReg:
        def resolve(self, task_id: str) -> SimpleNamespace:
            return SimpleNamespace(task_id=task_id, robot_id=config.worker.robot_id)

        def create_task(self, task_id: str, cfg: object) -> DirectTask:
            del task_id, cfg
            return _UnknownExportTask([])

        def create_curriculum(self, *args: object, **kwargs: object) -> None:
            del args, kwargs
            return None

    monkeypatch.setattr(export_cli, "find_run_directory_for_checkpoint", lambda _: run_dir)
    monkeypatch.setattr(export_cli, "load_run_config_source", lambda *args, **kwargs: source)
    monkeypatch.setattr("uerl.tasks.registry.create_default_registry", lambda: _CartPoleReg())
    monkeypatch.setattr(
        "uerl.training.robot_runtime_from_config",
        lambda cfg: RobotRuntime(
            actuators=(
                RobotRuntimeActuator(
                    joint="cart",
                    stiffness=0.0,
                    damping=1.0,
                    effort_limit=100.0,
                    default_position=0.0,
                ),
            )
        ),
    )
    return run_dir, checkpoint, output


def test_offline_export_skips_session_open_and_host_flags(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """When export_contract.yaml is present, export skips UE launch and host lookup."""

    _, checkpoint, output = _prepare_saved_run_with_contract(monkeypatch, tmp_path)

    def unexpected_open(*args: object, **kwargs: object) -> None:
        del args, kwargs
        pytest.fail("offline export must not open UERLSession")

    def unexpected_host(*args: object, **kwargs: object) -> None:
        del args, kwargs
        pytest.fail("offline export must not resolve host flags")

    fake_artifact = SimpleNamespace(
        task_id=CARTPOLE_TASK_ID,
        robot_id="uerl.robot.cartpole",
        onnx=b"fake-onnx-bytes",
    )
    captured: dict[str, object] = {}

    def fake_offline_export(**kwargs: object) -> SimpleNamespace:
        captured.update(kwargs)
        return fake_artifact

    monkeypatch.setattr(session_module.UERLSession, "open", classmethod(unexpected_open))
    monkeypatch.setattr(export_cli, "resolve_host_flags", unexpected_host)
    monkeypatch.setattr(export_cli, "export_policy_offline", fake_offline_export)

    result = export_cli.main(["--checkpoint", str(checkpoint), "--output", str(output)])

    assert result == 0
    assert captured["checkpoint"] == checkpoint
    assert captured["output"] == output
    assert "mode=offline" in capsys.readouterr().out


def test_online_flag_forces_session_open_even_with_export_contract(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """Passing --online bypasses export_contract.yaml and opens a UE Session."""

    _, checkpoint, output = _prepare_saved_run_with_contract(monkeypatch, tmp_path)
    events: list[str] = []

    monkeypatch.setattr(
        export_cli,
        "resolve_host_flags",
        lambda args: SimpleNamespace(ue_executable=tmp_path / "ue.exe", project=tmp_path / "proj.uproject"),
    )
    monkeypatch.setattr(
        "uerl.training.build_launch_overrides",
        lambda **kwargs: {"session.worker_args": "-uerlport=15555"},
    )

    class _FakeSession:
        def close(self, reason: str = "close") -> None:
            del reason
            events.append("session.close")

    def open_session(cls: object, /, config: object, **kwargs: object) -> _FakeSession:
        del cls, config, kwargs
        events.append("session.open")
        return _FakeSession()

    class _FakeDirectEnv:
        def __init__(self, *args: object, **kwargs: object) -> None:
            del args, kwargs
            events.append("direct_env.init")

        def close(self, reason: str) -> None:
            del reason
            events.append("direct_env.close")

    monkeypatch.setattr(session_module.UERLSession, "open", classmethod(open_session))
    monkeypatch.setattr(session_module, "UERLSessionAdapter", lambda session, device: session)
    monkeypatch.setattr(direct_env_module, "UERLDirectEnv", _FakeDirectEnv)

    result = export_cli.main(["--checkpoint", str(checkpoint), "--output", str(output), "--online"])

    assert result == 1
    assert events == ["session.open", "direct_env.init", "direct_env.close"]


def test_offline_export_rejects_mismatched_contract_task_id(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Offline export refuses an export_contract.yaml whose task_id differs from the Run."""

    _, checkpoint, output = _prepare_saved_run_with_contract(
        monkeypatch,
        tmp_path,
        contract_task_id="Other-Task-v0",
    )

    result = export_cli.main(["--checkpoint", str(checkpoint), "--output", str(output)])

    assert result == 1
    assert "does not match resolved Run" in capsys.readouterr().out

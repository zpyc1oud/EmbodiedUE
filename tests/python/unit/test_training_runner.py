"""Verify the generic training assembly and direct option boundary."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from typing import Any, cast
from unittest.mock import Mock

import pytest

from uerl import (
    ConfigError,
    DirectTask,
    DirectTaskConfig,
    LaunchMode,
    LoggingConfig,
    PresentationMode,
    ResolvedRunConfig,
    RslRlRunnerConfig,
    SessionConfig,
    WorkerConfig,
)
from uerl.core.config.snapshot import decode_worker_args
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.tasks.phantomx.evaluation import PhantomXEvaluationResult
from uerl.training import (
    build_launch_overrides,
    build_rsl_rl_train_config,
    build_run_config,
    parse_config_options,
    run_evaluation,
    run_training,
)


def _config(run_directory: Path) -> ResolvedRunConfig:
    """Build a resolved Config suitable for mocked lifecycle tests."""

    return ResolvedRunConfig(
        task_id="Example-Task-v0",
        task_version="1.0",
        session=SessionConfig(mode=LaunchMode.ATTACH),
        worker=WorkerConfig(environment_id="environment", robot_id="robot", run_seed=7),
        task=DirectTaskConfig(state_requirements=("state.value",)),
        runner=RslRlRunnerConfig(
            rollout_length=16,
            max_iterations=7,
            checkpoint=run_directory / "custom.pt",
            parameters={"max_iterations": 999, "experiment_name": "example"},
        ),
        logging=LoggingConfig(run_directory=run_directory),
        normalized_hash="a" * 64,
    )


def test_parse_config_options_uses_direct_path_value_syntax() -> None:
    """Keep raw values intact while accepting repeated dotted options."""

    assert parse_config_options(
        ["--worker.slot_count", "32", "--runner.max_iterations", "150"]
    ) == {"worker.slot_count": "32", "runner.max_iterations": "150"}
    print("[VERIFY] VC-002: direct_path=PASS raw_values=PASS")


@pytest.mark.parametrize(
    "entries",
    [
        ["--worker.slot_count"],
        ["--worker.slot_count", "1", "--worker.slot_count", "2"],
        ["--override", "runner.max_iterations=1"],
        ["--runner.max_iterations=1"],
        ["--iterations", "1"],
    ],
)
def test_parse_config_options_rejects_non_contract_syntax(entries: list[str]) -> None:
    """Reject missing values, duplicates, equals syntax, and task flags."""

    with pytest.raises(ValueError):
        parse_config_options(entries)


def test_build_run_config_resolves_max_iterations_as_typed_field(tmp_path: Path) -> None:
    """Route a direct max-iteration path through the Resolver and hash."""

    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={
            "runner.max_iterations": "3",
            "logging.run_directory": str(tmp_path / "run"),
        },
    )

    assert config.runner.max_iterations == 3
    assert "max_iterations" not in config.runner.parameters
    assert config.logging.run_directory == tmp_path / "run"
    assert len(config.normalized_hash) == 64
    print(f"[VERIFY] VC-002: max_iterations={config.runner.max_iterations} hash=PASS")


def test_launch_overrides_keep_worker_port_in_launch_arguments(tmp_path: Path) -> None:
    """Build one generic launch mapping without task-specific values."""

    overrides = build_launch_overrides(
        ue_executable=tmp_path / "UnrealEditor-Cmd.exe",
        project=tmp_path / "UERLHost.uproject",
        map_name="/Engine/Maps/Entry",
        port=45678,
    )

    assert overrides["session.port"] == "45678"
    assert overrides["session.map_path"] == "/Engine/Maps/Entry"
    assert "-uerlport=45678" in decode_worker_args(overrides["session.worker_args"])


def test_launch_overrides_default_to_headless_null_rhi(tmp_path: Path) -> None:
    """Keep the default Worker headless with the UE-required none-mode flags."""

    overrides = build_launch_overrides(
        ue_executable=tmp_path / "UnrealEditor-Cmd.exe",
        project=tmp_path / "UERLHost.uproject",
        map_name="/Engine/Maps/Entry",
        port=1,
    )

    worker_args = decode_worker_args(overrides["session.worker_args"])
    assert overrides["session.presentation_mode"] == "none"
    assert "-uerlpresentation=none" in worker_args
    assert "-nullrhi" in worker_args
    assert "-unattended" in worker_args
    assert "-ini:Engine:[/Script/Engine.PhysicsSettings]:bSubstepping=False" in worker_args


def test_project_default_engine_uses_deploy_substepping() -> None:
    """Play uses synchronous Chaos substeps; Worker launch overrides lockstep."""

    ini = Path(__file__).resolve().parents[3] / "engine" / "Config" / "DefaultEngine.ini"
    text = ini.read_text(encoding="utf-8")
    assert "bSubstepping=True" in text
    assert "MaxSubstepDeltaTime=0.005" in text
    assert "MaxSubsteps=7" in text


def test_launch_overrides_viewport_open_a_window_without_null_rhi(tmp_path: Path) -> None:
    """Emit windowed viewport flags and drop the RHI-disabling headless flags."""

    overrides = build_launch_overrides(
        ue_executable=tmp_path / "UnrealEditor-Cmd.exe",
        project=tmp_path / "UERLHost.uproject",
        map_name="/Engine/Maps/Entry",
        port=1,
        presentation=PresentationMode.VIEWPORT,
        window_size=(800, 600),
    )

    worker_args = decode_worker_args(overrides["session.worker_args"])
    assert overrides["session.presentation_mode"] == "viewport"
    assert "-uerlpresentation=viewport" in worker_args
    assert "-windowed" in worker_args
    assert "-ResX=800" in worker_args
    assert "-ResY=600" in worker_args
    assert "-uerlfollowrobot=1" in worker_args
    assert "-nullrhi" not in worker_args
    assert "-unattended" not in worker_args


def test_launch_overrides_gameplay_preserve_map_player_without_observer(tmp_path: Path) -> None:
    installed_map = tmp_path / "Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap"
    installed_map.parent.mkdir(parents=True)
    installed_map.write_bytes(b"synthetic package presence fixture")
    overrides = build_launch_overrides(
        ue_executable=tmp_path / "UnrealEditor-Cmd.exe",
        project=tmp_path / "UERLHost.uproject",
        map_name="/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo",
        port=1,
        presentation=PresentationMode.GAMEPLAY,
        window_size=(1280, 720),
    )

    worker_args = decode_worker_args(overrides["session.worker_args"])
    assert overrides["session.presentation_mode"] == "gameplay"
    assert "-uerlpresentation=gameplay" in worker_args
    assert "-windowed" in worker_args
    assert "-uerlfollowrobot=1" not in worker_args
    assert "-nullrhi" not in worker_args


def test_rsl_rl_config_derives_generic_names_and_ignores_dynamic_iterations(tmp_path: Path) -> None:
    """Keep task-specific naming optional and max iterations out of RSL-RL data."""

    train_config = build_rsl_rl_train_config(_config(tmp_path / "run"))

    assert train_config["experiment_name"] == "example"
    assert train_config["run_name"] == "example_task_v0"
    assert "max_iterations" not in train_config
    print("[VERIFY] VC-005: task_neutral_names=PASS dynamic_iterations=IGNORED")


def test_run_training_uses_config_iterations_and_closes_wrapper(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """Drive the generic lifecycle with seams and verify one iteration source."""

    import uerl.training.runner as runner_module

    raw_session = Mock()
    task = Mock()
    registry = Mock()
    registry.create_task.return_value = task
    direct_env = Mock()
    vec_env = Mock()
    calls: dict[str, object] = {"order": []}
    order = cast(list[str], calls["order"])
    capabilities = Mock()
    capabilities.format.return_value = (
        "train=supported evaluate=supported export=supported"
    )
    capabilities.require.side_effect = lambda operation, **_kwargs: order.append(
        f"capability:{operation}"
    )
    task.capabilities = capabilities
    checkpoint = tmp_path / "run" / "custom.pt"
    checkpoint.parent.mkdir(parents=True)
    checkpoint.write_bytes(b"existing checkpoint")

    class _Runner:
        """Capture the RSL-RL calls without starting a real environment."""

        def __init__(self, _env: object, _cfg: object, *, log_dir: str, device: str) -> None:
            calls["log_dir"] = log_dir
            calls["device"] = device

        def learn(self, iterations: int, *, init_at_random_ep_len: bool = False) -> None:
            calls["iterations"] = iterations
            calls["init_at_random_ep_len"] = init_at_random_ep_len
            order.append("learn")

        def load(
            self,
            path: str,
            *,
            map_location: str,
            restore_curriculum: bool,
            skip_curriculum_terms: tuple[str, ...],
        ) -> None:
            calls["loaded_checkpoint"] = path
            calls["map_location"] = map_location
            calls["restore_curriculum"] = restore_curriculum
            calls["skip_curriculum_terms"] = skip_curriculum_terms
            order.append("load")

        def save(self, path: str) -> None:
            order.append("save")
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            Path(path).write_bytes(b"checkpoint")

    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", Mock(return_value=raw_session))
    monkeypatch.setattr(runner_module, "UERLSessionAdapter", lambda *_args, **_kwargs: Mock())
    monkeypatch.setattr(runner_module, "UERLDirectEnv", Mock(return_value=direct_env))
    monkeypatch.setattr(runner_module, "UERLVecEnvWrapper", Mock(return_value=vec_env))
    monkeypatch.setattr(runner_module, "UERLOnPolicyRunner", _Runner)

    result = run_training(_config(tmp_path / "run"))

    assert calls["iterations"] == 7
    assert calls["loaded_checkpoint"] == str(checkpoint)
    assert calls["map_location"] == "cpu"
    assert calls["restore_curriculum"] is True
    assert calls["skip_curriculum_terms"] == ()
    # Resume resets every Slot inside load; learn then staggers the timeouts.
    assert calls["order"] == [
        "capability:train",
        "capability:train",
        "load",
        "learn",
        "save",
    ]
    assert calls["init_at_random_ep_len"] is True
    assert result.iterations == 7
    assert result.checkpoint.is_file()
    vec_env.close.assert_called_once_with("training_complete")
    assert "[CAPABILITY] train=supported evaluate=supported export=supported" in capsys.readouterr().out
    print("[VERIFY] VC-004: max_iterations=7 source=runner.max_iterations")


def test_run_training_preflights_missing_task_methods_before_opening_session(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """A Task without action/observation/reward paths fails before UE startup."""

    import uerl.training.runner as runner_module

    registry = Mock()
    registry.create_task.return_value = DirectTask()
    open_session = Mock(side_effect=AssertionError("capability check must precede UE startup"))
    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", open_session)
    config = _config(tmp_path / "run")
    config = replace(config, runner=replace(config.runner, checkpoint=None))

    with pytest.raises(ConfigError) as raised:
        run_training(config)

    assert raised.value.code == "TASK_CAPABILITY_UNSUPPORTED"
    assert raised.value.path == "task.capabilities.train"
    open_session.assert_not_called()


def test_run_evaluation_preflights_missing_task_methods_before_opening_session(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """Evaluation uses the same DirectTask contract and rejects it before UE."""

    import uerl.training.runner as runner_module

    checkpoint = tmp_path / "checkpoint.pt"
    checkpoint.write_bytes(b"checkpoint")
    registry = Mock()
    registry.create_task.return_value = DirectTask()
    open_session = Mock(side_effect=AssertionError("capability check must precede UE startup"))
    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", open_session)

    with pytest.raises(ConfigError) as raised:
        run_evaluation(_config(tmp_path / "run"), checkpoint=checkpoint, steps=1)

    assert raised.value.code == "TASK_CAPABILITY_UNSUPPORTED"
    assert raised.value.path == "task.capabilities.evaluate"
    open_session.assert_not_called()


def test_run_training_keeps_environment_cpu_when_runner_uses_cuda(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """Keep UE/Task tensors on CPU while the RSL-RL runner targets CUDA."""

    import uerl.training.runner as runner_module

    config = _config(tmp_path / "run")
    config = replace(config, runner=replace(config.runner, device="cuda:0", checkpoint=None))
    raw_session = Mock()
    registry = Mock()
    registry.create_task.return_value = Mock()
    direct_env = Mock()
    vec_env = Mock()
    adapter_factory = Mock(return_value=Mock())
    direct_env_factory = Mock(return_value=direct_env)
    calls: dict[str, object] = {}

    class _Runner:
        def __init__(self, _env: object, _cfg: object, *, log_dir: str, device: str) -> None:
            calls["device"] = device

        def learn(self, _iterations: int, *, init_at_random_ep_len: bool = False) -> None:
            del init_at_random_ep_len

        def save(self, path: str) -> None:
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            Path(path).write_bytes(b"checkpoint")

    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", Mock(return_value=raw_session))
    monkeypatch.setattr(runner_module, "UERLSessionAdapter", adapter_factory)
    monkeypatch.setattr(runner_module, "UERLDirectEnv", direct_env_factory)
    monkeypatch.setattr(runner_module, "UERLVecEnvWrapper", Mock(return_value=vec_env))
    monkeypatch.setattr(runner_module, "UERLOnPolicyRunner", _Runner)

    run_training(config)

    assert adapter_factory.call_args.kwargs["device"] == "cpu"
    assert direct_env_factory.call_args.kwargs["device"] == "cpu"
    assert registry.create_curriculum.call_args.kwargs["device"] == "cpu"
    assert calls["device"] == "cuda:0"
    vec_env.close.assert_called_once_with("training_complete")


def test_run_training_can_fix_all_slots_to_one_terrain_level(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """Disable automatic terrain promotion when a fixed level is requested."""

    import uerl.training.runner as runner_module

    config = _config(tmp_path / "run")
    config = replace(
        config,
        worker=replace(config.worker, terrain_config={"num_levels": 4}),
    )
    checkpoint = cast(Path, config.runner.checkpoint)
    checkpoint.parent.mkdir(parents=True)
    checkpoint.write_bytes(b"adaptive terrain checkpoint")
    raw_session = Mock()
    registry = Mock()
    registry.create_task.return_value = Mock()
    direct_env = Mock()
    vec_env = Mock()
    load_kwargs: dict[str, object] = {}

    class _Runner:
        def __init__(self, _env: object, _cfg: object, *, log_dir: str, device: str) -> None:
            del log_dir, device

        def load(self, _path: str, **kwargs: object) -> None:
            load_kwargs.update(kwargs)

        def learn(self, _iterations: int, *, init_at_random_ep_len: bool = False) -> None:
            del init_at_random_ep_len

        def save(self, path: str) -> None:
            Path(path).parent.mkdir(parents=True, exist_ok=True)
            Path(path).write_bytes(b"checkpoint")

    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", Mock(return_value=raw_session))
    monkeypatch.setattr(runner_module, "UERLSessionAdapter", lambda *_args, **_kwargs: Mock())
    direct_env_factory = Mock(return_value=direct_env)
    monkeypatch.setattr(runner_module, "UERLDirectEnv", direct_env_factory)
    monkeypatch.setattr(runner_module, "UERLVecEnvWrapper", Mock(return_value=vec_env))
    monkeypatch.setattr(runner_module, "UERLOnPolicyRunner", _Runner)

    run_training(config, terrain_level=3)

    assert direct_env_factory.call_args.kwargs["initial_terrain_level"] == 3
    assert direct_env_factory.call_args.kwargs.get("curriculum") is None
    assert load_kwargs["restore_curriculum"] is True
    assert load_kwargs["skip_curriculum_terms"] == ("terrain",)
    vec_env.close.assert_called_once_with("training_complete")


def test_run_training_closes_raw_session_when_env_setup_fails(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """Close the acquired Session when DirectEnv setup raises."""

    import uerl.training.runner as runner_module

    raw_session = Mock()
    registry = Mock()
    registry.create_task.return_value = Mock()

    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", Mock(return_value=raw_session))
    monkeypatch.setattr(runner_module, "UERLSessionAdapter", lambda *_args, **_kwargs: Mock())
    monkeypatch.setattr(runner_module, "UERLDirectEnv", Mock(side_effect=RuntimeError("env setup")))

    with pytest.raises(RuntimeError, match="env setup"):
        config = _config(tmp_path / "run")
        run_training(replace(config, runner=replace(config.runner, checkpoint=None)))

    raw_session.close.assert_called_once_with("training_setup_failed")
    print("[VERIFY] VC-008: failure_cleanup=PASS")


def test_run_evaluation_loads_checkpoint_and_aggregates_completed_episodes(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """Evaluate deterministic actions and report episode-level fall metrics."""

    import torch
    from tensordict import TensorDict

    import uerl.training.runner as runner_module

    checkpoint = tmp_path / "model.pt"
    checkpoint.write_bytes(b"checkpoint")
    raw_session = Mock()
    registry = Mock()
    registry.create_task.return_value = Mock()
    from uerl.tasks.phantomx.evaluation import PhantomXEvaluator

    registry.create_evaluator.return_value = PhantomXEvaluator(sample_hz=50.0)
    direct_env = Mock()
    vec_env = Mock()
    vec_env.device = "cpu"
    vec_env.get_observations.return_value = TensorDict(
        {"policy": torch.zeros(2, 66)}, batch_size=[2]
    )
    vec_env.step.side_effect = [
        (
            TensorDict({"policy": torch.ones(2, 66)}, batch_size=[2]),
            torch.zeros(2),
            torch.tensor([True, True]),
            {
                "terminal_episode_length": torch.tensor([10, 1]),
                "transition_dt": 0.005,
                "log": {
                    "phantomx/forward_velocity": torch.tensor([0.2, 0.4]),
                    "phantomx/linear_velocity_error": torch.tensor([0.3, 0.1]),
                    "phantomx/command_vx": torch.tensor([0.4, 0.4]),
                    "phantomx/command_speed": torch.tensor([0.4, 0.4]),
                    "phantomx/action_clip_fraction": torch.tensor([0.0, 0.0]),
                    "phantomx/torque_clip_fraction": torch.tensor([0.1, 0.2]),
                    "phantomx/torque_over_limit": torch.tensor([0.0, 0.1]),
                    "phantomx/action_rate": torch.tensor([0.04, 0.16]),
                    "phantomx/body_height": torch.tensor([0.16, 0.17]),
                    "body_clearance": torch.tensor([0.02, 0.10]),
                    "upright": torch.tensor([0.4, 0.9]),
                    "reset_age_steps": torch.tensor([1.0, 1.0]),
                    "phantomx/reset_event": torch.tensor([1.0, 0.0]),
                    "phantomx/reset_joint_error_max": torch.tensor([0.01, 0.0]),
                    "phantomx/displacement": torch.tensor([0.0, 0.0]),
                    "base_contact": torch.tensor([True, False]),
                    "timeout": torch.tensor([False, True]),
                },
            },
        ),
        (
            TensorDict({"policy": torch.full((2, 66), 2.0)}, batch_size=[2]),
            torch.zeros(2),
            torch.tensor([False, True]),
            {
                "terminal_episode_length": torch.tensor([1, 20]),
                "transition_dt": 0.035,
                "log": {
                    "phantomx/forward_velocity": torch.tensor([0.4, 0.6]),
                    "phantomx/linear_velocity_error": torch.tensor([0.1, 0.1]),
                    "phantomx/command_vx": torch.tensor([0.4, 0.4]),
                    "phantomx/command_speed": torch.tensor([0.4, 0.4]),
                    "phantomx/action_clip_fraction": torch.tensor([0.0, 0.0]),
                    "phantomx/torque_clip_fraction": torch.tensor([0.0, 0.0]),
                    "phantomx/torque_over_limit": torch.tensor([0.0, 0.0]),
                    "phantomx/action_rate": torch.tensor([0.04, 0.16]),
                    "phantomx/body_height": torch.tensor([0.16, 0.17]),
                    "body_clearance": torch.tensor([0.10, 0.03]),
                    "upright": torch.tensor([0.9, 0.9]),
                    "reset_age_steps": torch.tensor([1.0, 20.0]),
                    "phantomx/reset_event": torch.tensor([0.0, 1.0]),
                    "phantomx/reset_joint_error_max": torch.tensor([0.0, 0.02]),
                    "phantomx/displacement": torch.tensor([0.0, 0.0]),
                    "base_contact": torch.tensor([False, False]),
                    "timeout": torch.tensor([False, True]),
                },
            },
        ),
    ]
    calls: dict[str, object] = {"observations": []}

    class _Policy:
        def __call__(self, observations: object) -> torch.Tensor:
            observations_list = cast(list[object], calls["observations"])
            observations_list.append(observations)
            return torch.zeros(2, 1)

    class _Runner:
        def __init__(self, _env: object, _cfg: object, *, log_dir: None, device: str) -> None:
            calls["log_dir"] = log_dir
            calls["device"] = device

        def load(self, path: str, *, map_location: str) -> None:
            calls["checkpoint"] = path
            calls["map_location"] = map_location

        def get_inference_policy(self, *, device: str) -> _Policy:
            calls["policy_device"] = device
            return _Policy()

    direct_env_type = Mock(return_value=direct_env)
    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", Mock(return_value=raw_session))
    monkeypatch.setattr(runner_module, "UERLSessionAdapter", lambda *_args, **_kwargs: Mock())
    monkeypatch.setattr(runner_module, "UERLDirectEnv", direct_env_type)
    monkeypatch.setattr(runner_module, "UERLVecEnvWrapper", Mock(return_value=vec_env))
    monkeypatch.setattr(runner_module, "UERLOnPolicyRunner", _Runner)

    result = cast(
        PhantomXEvaluationResult,
        run_evaluation(_config(tmp_path / "run"), checkpoint=checkpoint, steps=2),
    )

    assert calls["checkpoint"] == str(checkpoint)
    registry.create_task.return_value.enable_observation_corruption.assert_not_called()
    assert direct_env_type.call_args.kwargs.get("event_manager_factory") is None
    assert calls["log_dir"] is None
    assert result.completed_episodes == 3
    assert result.mean_forward_velocity == pytest.approx(0.4)
    assert result.mean_speed_error == pytest.approx(0.15)
    assert result.success_rate == pytest.approx(2.0 / 3.0)
    assert result.mean_command_vx == pytest.approx(0.4)
    assert result.mean_command_speed == pytest.approx(0.4)
    assert result.mean_actor_command_vx == pytest.approx(0.5)
    assert result.mean_actor_command_speed == pytest.approx(2**0.5 / 2)
    assert result.mean_command_observation_error == pytest.approx(0.5)
    assert result.mean_action_clip_fraction == pytest.approx(0.0)
    assert result.mean_torque_clip_fraction == pytest.approx(0.075)
    assert result.mean_torque_over_limit == pytest.approx(0.025)
    assert result.fall_rate == pytest.approx(1.0 / 3.0)
    assert result.base_contact_rate == pytest.approx(1.0 / 3.0)
    assert len(result.termination_events) == 3
    expected_events = (
        (0, 1, 0.02, 0.4, "base_contact"),
        (1, 1, 0.10, 0.9, "timeout"),
        (1, 20, 0.03, 0.9, "timeout"),
    )
    for event, expected in zip(result.termination_events, expected_events, strict=True):
        slot, age, clearance, upright, reason = expected
        assert event["slot"] == slot
        assert event["reset_age_steps"] == age
        assert event["ground_clearance"] == pytest.approx(clearance)
        assert event["upright"] == pytest.approx(upright)
        assert event["termination_reason"] == reason
    assert result.mean_episode_length == pytest.approx(31.0 / 3.0)
    assert result.mean_action_rate == pytest.approx(0.1)
    assert result.mean_body_height == pytest.approx(0.165)
    assert result.mean_reset_joint_error == pytest.approx(0.015)
    assert result.dominant_body_height_frequency_hz == pytest.approx(0.0)
    vec_env.close.assert_called_once_with("evaluation_complete")


def test_dominant_frequency_ignores_constant_signal_and_finds_peak() -> None:
    """Keep gait frequency instrumentation tied to sampled physical time."""

    import math

    from uerl.tasks.phantomx.evaluation import _dominant_frequency_hz

    sample_hz = 50.0
    values = [math.sin(2.0 * math.pi * 5.0 * index / sample_hz) for index in range(100)]
    assert _dominant_frequency_hz(values, sample_hz=sample_hz) == pytest.approx(5.0)
    assert _dominant_frequency_hz([1.0] * 100, sample_hz=sample_hz) == 0.0


def test_ac_py_unit_eval_001_gait_frequency_uses_actual_variable_sample_times() -> None:
    """Alternating 5/35 ms observations retain a 2 Hz physical oscillation."""

    import math

    from uerl.tasks.phantomx.evaluation import _dominant_frequency_hz

    durations = [0.005, 0.035] * 50
    elapsed = 0.0
    heights: list[float] = []
    for duration in durations:
        elapsed += duration
        heights.append(math.sin(2.0 * math.pi * 2.0 * elapsed))

    frequency = _dominant_frequency_hz(heights, sample_hz=1.0 / 0.035, sample_dts=durations)
    assert frequency == pytest.approx(2.0, abs=0.15)


def test_run_training_rejects_missing_resume_before_opening_session(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """An explicit resume path must never silently start a fresh training run."""
    import uerl.training.runner as runner_module

    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={"runner.checkpoint": str(tmp_path / "missing.pt")},
    )
    open_session = Mock(side_effect=AssertionError("must not launch UE"))
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", open_session)

    with pytest.raises(FileNotFoundError, match="missing.pt"):
        run_training(config)

    open_session.assert_not_called()


@pytest.mark.parametrize("contents", [None, b"", b"version https://git-lfs.github.com/spec/v1\n"])
def test_optional_egypt_missing_map_fails_before_launch_arguments(tmp_path: Path, contents: bytes | None) -> None:
    map_file = tmp_path / "Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap"
    if contents is not None:
        map_file.parent.mkdir(parents=True)
        map_file.write_bytes(contents)
    with pytest.raises(FileNotFoundError, match="Acquire/install your own copy"):
        build_launch_overrides(
            ue_executable=tmp_path / "must-not-launch.exe",
            project=tmp_path / "UERLHost.uproject",
            map_name="/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo",
            port=1,
        )


def test_default_host_map_does_not_require_optional_fab_content() -> None:
    root = Path(__file__).resolve().parents[3]
    text = (root / "engine/Config/DefaultEngine.ini").read_text()
    assert "GameDefaultMap=/Engine/Maps/Entry" in text

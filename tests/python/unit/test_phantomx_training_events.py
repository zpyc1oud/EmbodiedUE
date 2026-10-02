"""Training-only PhantomX friction and root-push events."""

from __future__ import annotations

from dataclasses import replace
from types import SimpleNamespace
from typing import Any, cast
from unittest.mock import Mock

import pytest
import torch

from uerl.core.config.robot import RobotSpec, RobotTopology
from uerl.core.mdp.lib.events import RecordingEventBridge
from uerl.tasks.cartpole.config import CARTPOLE_TASK_ID
from uerl.tasks.phantomx.config import (
    PHANTOMX_DISCRETE_TERRAIN_TASK_ID,
    PHANTOMX_PURSUIT_TASK_ID,
    PHANTOMX_TASK_ID,
    PHANTOMX_TERRAIN_TASK_ID,
    load_phantomx_training_config,
)
from uerl.tasks.phantomx.registration import create_phantomx_event_manager
from uerl.tasks.registry import create_default_registry
from uerl.training.runner import run_training


def _robot_spec() -> RobotSpec:
    topology = RobotTopology(
        body_names=("root",),
        body_motion_types=("simulated",),
        root_body_index=0,
        fixed_base=False,
        joints=(),
    )
    return RobotSpec(actuators=(), observations=(), reset=(), body_names=("root",), topology=topology)


def test_phantomx_training_config_publishes_event_and_noise_ranges() -> None:
    task = load_phantomx_training_config().task

    assert task.command.resampling_time_min_s == pytest.approx(5.0)
    assert task.command.resampling_time_max_s == pytest.approx(10.0)
    assert task.command.standing_probability == pytest.approx(0.1)
    assert (task.events.static_friction_min, task.events.static_friction_max) == pytest.approx((0.7, 1.1))
    assert (task.events.dynamic_friction_min, task.events.dynamic_friction_max) == pytest.approx((0.5, 0.9))
    assert (task.events.push_velocity_min_mps, task.events.push_velocity_max_mps) == pytest.approx((-0.2, 0.2))
    assert (task.events.push_interval_min_s, task.events.push_interval_max_s) == pytest.approx((1.0, 1.5))
    assert task.observation_noise.lin_vel_b == pytest.approx(0.05)
    assert task.observation_noise.joint_vel == pytest.approx(0.05)


def test_phantomx_training_events_are_friction_and_root_push_only() -> None:
    config = load_phantomx_training_config().task
    manager = create_phantomx_event_manager(
        config,
        _robot_spec(),
        batch_size=4,
        device="cpu",
        run_seed=3,
    )
    bridge = RecordingEventBridge()
    manager.bind_bridge(bridge)

    assert manager.term_modes == {"ground_friction": "startup", "root_push": "interval"}
    manager.apply("startup")
    effect = bridge.drain_effects()[0]
    assert effect.kind == "ground_friction"
    assert torch.all((effect.values[:, 0] >= 0.7) & (effect.values[:, 0] <= 1.1))
    assert torch.all((effect.values[:, 1] >= 0.5) & (effect.values[:, 1] <= 0.9))


def test_only_phantomx_tasks_register_training_events() -> None:
    registry = create_default_registry()

    assert registry.resolve(CARTPOLE_TASK_ID).event_manager_factory is None
    for task_id in (
        PHANTOMX_TASK_ID,
        PHANTOMX_TERRAIN_TASK_ID,
        PHANTOMX_DISCRETE_TERRAIN_TASK_ID,
        PHANTOMX_PURSUIT_TASK_ID,
    ):
        assert registry.resolve(task_id).event_manager_factory is create_phantomx_event_manager


def test_run_training_enables_noise_and_passes_the_registered_event_factory(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Any,
) -> None:
    import uerl.training.runner as runner_module

    seen: dict[str, object] = {}

    def factory(config: object, spec: object, *, batch_size: int, device: str, run_seed: int) -> Mock:
        del config, spec
        seen["batch"] = (batch_size, device, run_seed)
        return Mock()

    task = Mock()
    registry = Mock()
    registry.create_task.return_value = task
    registry.resolve.return_value = SimpleNamespace(event_manager_factory=factory)
    registry.create_curriculum.return_value = None
    direct_env = Mock()

    class _Runner:
        def __init__(self, _env: object, _cfg: object, *, log_dir: str, device: str) -> None:
            del _env, _cfg, log_dir, device

        def learn(self, _iterations: int, *, init_at_random_ep_len: bool = False) -> None:
            del init_at_random_ep_len

        def save(self, path: str) -> None:
            from pathlib import Path

            Path(path).parent.mkdir(parents=True, exist_ok=True)
            Path(path).write_bytes(b"checkpoint")

    direct_env_type = Mock(return_value=direct_env)
    monkeypatch.setattr(runner_module, "create_default_registry", lambda: registry)
    monkeypatch.setattr(cast(Any, runner_module).UERLSession, "open", Mock(return_value=Mock()))
    monkeypatch.setattr(runner_module, "UERLSessionAdapter", lambda *_args, **_kwargs: Mock())
    monkeypatch.setattr(runner_module, "UERLDirectEnv", direct_env_type)
    monkeypatch.setattr(runner_module, "UERLVecEnvWrapper", Mock(return_value=Mock()))
    monkeypatch.setattr(runner_module, "UERLOnPolicyRunner", _Runner)
    monkeypatch.setattr(runner_module, "_attach_terrain_curriculum", lambda *_args, **_kwargs: None)

    from tests.python.unit.test_training_runner import _config

    config = _config(tmp_path / "run")
    config = replace(config, runner=replace(config.runner, checkpoint=None))
    run_training(config)

    task.enable_observation_corruption.assert_called_once()
    bound = direct_env_type.call_args.kwargs["event_manager_factory"]
    bound(Mock())
    assert seen["batch"] == (config.worker.slot_count, "cpu", config.worker.run_seed)


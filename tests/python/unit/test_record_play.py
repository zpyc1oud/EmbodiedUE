"""Play owns recording, one robot, and the named controller."""

from __future__ import annotations

from importlib.metadata import entry_points
from pathlib import Path

import pytest
import torch

from tests.python.unit.test_phantomx_task import SHAPES, _robot_spec, _state
from uerl.cli import export, play, train
from uerl.errors import ConfigError
from uerl.tasks.cartpole import create_cartpole_task, create_cartpole_task_config
from uerl.tasks.controllers import (
    PlayerVelocityController,
    create_play_controller,
    register_play_controller,
)
from uerl.tasks.evaluation import EvaluationSummary
from uerl.tasks.phantomx.config import PhantomXTaskConfig
from uerl.tasks.phantomx.task import PhantomXTask


def test_installed_console_entrypoint_is_uerl() -> None:
    names = {entry_point.name for entry_point in entry_points(group="console_scripts")}
    retired = {"uerl-play", "uerl-train", "uerl-export", "uerl-tasks", "uerl-config", "uerl-record-play"}

    assert retired.isdisjoint(names)
    entry_point = next(item for item in entry_points(group="console_scripts") if item.name == "uerl")
    assert entry_point.value == "uerl.cli.main:main"
    assert callable(entry_point.load())


def test_play_rejects_more_than_one_slot() -> None:
    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                "UERL-PhantomX-Walk-v0",
                "--checkpoint",
                "model.pt",
                "--worker.slot_count",
                "2",
            ]
        )


def test_play_rejects_an_unknown_controller_before_launch() -> None:
    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                "UERL-PhantomX-Walk-v0",
                "--checkpoint",
                "model.pt",
                "--controller",
                "missing",
            ]
        )


def test_play_record_requires_a_new_viewport() -> None:
    from uerl.tasks.cartpole.config import CARTPOLE_TASK_ID

    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                CARTPOLE_TASK_ID,
                "--checkpoint",
                "model.pt",
                "--record",
                "walk.mp4",
                "--presentation",
                "gameplay",
            ]
        )
    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                CARTPOLE_TASK_ID,
                "--checkpoint",
                "model.pt",
                "--record",
                "walk.mp4",
                "--session.mode",
                "attach",
            ]
        )


def test_play_forces_one_slot_and_keeps_the_task_controller(monkeypatch: pytest.MonkeyPatch) -> None:
    from uerl.tasks.cartpole.config import CARTPOLE_TASK_ID

    seen: dict[str, object] = {}

    def fake_run(config: object, **kwargs: object) -> EvaluationSummary:
        seen["slots"] = config.worker.slot_count  # type: ignore[attr-defined]
        seen["controller"] = kwargs["play_controller"]
        seen["terrain_level"] = kwargs["terrain_level"]
        return EvaluationSummary(
            task_id=CARTPOLE_TASK_ID,
            checkpoint=Path("model.pt"),
            steps=500,
            completed_episodes=0,
            mean_episode_length=0.0,
            mean_reward=0.0,
        )

    monkeypatch.setattr("uerl.training.run_evaluation", fake_run)
    play.main(["--task", CARTPOLE_TASK_ID, "--checkpoint", "model.pt"])

    assert seen == {"slots": 1, "controller": "task", "terrain_level": None}


def test_train_and_export_do_not_accept_a_play_controller() -> None:
    assert "controller" not in {action.dest for action in train._parser()._actions}
    assert "controller" not in {action.dest for action in export._parser()._actions}


def test_cartpole_rejects_player_control_before_a_session() -> None:
    task = create_cartpole_task(create_cartpole_task_config())

    assert task._command_source.channels() == {}
    with pytest.raises(ConfigError, match="velocity command channel"):
        create_play_controller("player", task, 1)


def test_player_holds_default_targets_after_releasing_walk() -> None:
    player = PlayerVelocityController(speed_mps=0.5, max_yaw_rate=1.0, batch_size=1)
    policy_actions = torch.full((1, 18), 0.75)

    player.set_held({"W", "Q"})
    player.update({})
    assert torch.equal(player.gate_actions(policy_actions), policy_actions)

    player.set_held({"W", "S"})
    player.update({})
    assert torch.equal(player.gate_actions(policy_actions), torch.zeros_like(policy_actions))

    player.set_held(set())
    player.update({})
    assert torch.equal(player.gate_actions(policy_actions), torch.zeros_like(policy_actions))


def test_custom_controller_publishes_its_velocity_into_the_policy_observation() -> None:
    expected = torch.tensor([[0.2, -0.1, 0.3]])

    class _Scripted:
        def channels(self) -> dict[str, int]:
            return {"velocity": 3}

        def update(self, raw_state: object) -> None:
            del raw_state

        def reset(self, reset_mask: torch.Tensor, post_reset_state: object) -> None:
            del reset_mask, post_reset_state

        def current(self) -> dict[str, torch.Tensor]:
            return {"velocity": expected}

    register_play_controller("scripted-velocity", lambda task, batch_size: _Scripted())
    task = PhantomXTask(PhantomXTaskConfig(), control_dt=0.02, batch_size=1, device="cpu")
    replacement = create_play_controller("scripted-velocity", task, 1)
    assert replacement is not None
    task.use_command_source(replacement)
    task.bind_robot_spec(_robot_spec(), observation_shapes=SHAPES)
    state = _state(task, initial_command=torch.tensor([[0.9, 0.9]]))

    command = task.build_observations(state, torch.ones(1, dtype=torch.bool), torch.zeros(1, 18))[
        "policy"
    ][:, 9:12]

    assert torch.allclose(command, expected)


def test_custom_controller_width_mismatch_fails_at_assembly() -> None:
    class _Narrow:
        def channels(self) -> dict[str, int]:
            return {"velocity": 2}

        def current(self) -> dict[str, torch.Tensor]:
            return {"velocity": torch.zeros(1, 2)}

    register_play_controller("narrow-velocity", lambda task, batch_size: _Narrow())
    task = PhantomXTask(PhantomXTaskConfig(), control_dt=0.02, batch_size=1, device="cpu")
    replacement = create_play_controller("narrow-velocity", task, 1)
    assert replacement is not None
    task.use_command_source(replacement)

    with pytest.raises(ConfigError, match="width"):
        task.bind_robot_spec(_robot_spec(), observation_shapes=SHAPES)

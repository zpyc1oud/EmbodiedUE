"""Play owns recording, one robot, and the named controller."""

from __future__ import annotations

from importlib.metadata import entry_points
from pathlib import Path

import pytest
import torch

from tests.python.unit.test_phantomx_task import SHAPES, _robot_spec, _state
from uerl.cli import export, play, train
from uerl.core.config.canonical import to_jsonable
from uerl.errors import ConfigError
from uerl.tasks.cartpole import create_cartpole_task, create_cartpole_task_config
from uerl.tasks.controllers import (
    FixedVelocityController,
    PlayerVelocityController,
    create_play_controller,
    register_play_controller,
)
from uerl.tasks.evaluation import EvaluationSummary
from uerl.tasks.phantomx.config import PhantomXTaskConfig
from uerl.tasks.phantomx.task import PhantomXTask
from uerl.training import build_run_config


def _saved_checkpoint(tmp_path: Path, task_id: str) -> Path:
    run = tmp_path / "run"
    run.mkdir()
    checkpoint = run / "model.pt"
    torch.save({"infos": {}}, checkpoint)
    from tests.python.run_config_files import write_resolved_config

    write_resolved_config(run, to_jsonable(build_run_config(task_id)))
    return checkpoint


def test_installed_console_entrypoint_is_uerl() -> None:
    names = {entry_point.name for entry_point in entry_points(group="console_scripts")}
    retired = {"uerl-play", "uerl-train", "uerl-export", "uerl-tasks", "uerl-config", "uerl-record-play"}

    assert retired.isdisjoint(names)
    entry_point = next(item for item in entry_points(group="console_scripts") if item.name == "uerl")
    assert entry_point.value == "uerl.cli.main:main"
    assert callable(entry_point.load())


def test_play_rejects_more_than_one_slot(tmp_path: Path) -> None:
    checkpoint = _saved_checkpoint(tmp_path, "UERL-PhantomX-Walk-v0")
    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                "UERL-PhantomX-Walk-v0",
                "--checkpoint",
                str(checkpoint),
                "--worker.slot_count",
                "2",
            ]
        )


def test_play_rejects_an_unknown_controller_before_launch(tmp_path: Path) -> None:
    checkpoint = _saved_checkpoint(tmp_path, "UERL-PhantomX-Walk-v0")
    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                "UERL-PhantomX-Walk-v0",
                "--checkpoint",
                str(checkpoint),
                "--controller",
                "missing",
            ]
        )


def test_play_record_requires_a_new_viewport(tmp_path: Path) -> None:
    from uerl.tasks.cartpole.config import CARTPOLE_TASK_ID

    checkpoint = _saved_checkpoint(tmp_path, CARTPOLE_TASK_ID)
    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                CARTPOLE_TASK_ID,
                "--checkpoint",
                str(checkpoint),
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
                str(checkpoint),
                "--record",
                "walk.mp4",
                "--session.mode",
                "attach",
            ]
        )


def test_play_forces_one_slot_and_keeps_the_task_controller(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    from uerl.tasks.cartpole.config import CARTPOLE_TASK_ID

    seen: dict[str, object] = {}

    def fake_run(config: object, **kwargs: object) -> EvaluationSummary:
        seen["slots"] = config.worker.slot_count  # type: ignore[attr-defined]
        seen["controller"] = kwargs["play_controller"]
        seen["terrain_level"] = kwargs["terrain_level"]
        seen["restore_curriculum"] = kwargs["restore_curriculum"]
        seen["trace_path"] = kwargs["trace_path"]
        return EvaluationSummary(
            task_id=CARTPOLE_TASK_ID,
            checkpoint=Path("model.pt"),
            steps=500,
            completed_episodes=0,
            mean_episode_length=0.0,
            mean_reward=0.0,
        )

    monkeypatch.setattr("uerl.training.run_evaluation", fake_run)
    checkpoint = _saved_checkpoint(tmp_path, CARTPOLE_TASK_ID)
    play.main(["--task", CARTPOLE_TASK_ID, "--checkpoint", str(checkpoint)])

    assert seen == {
        "slots": 1,
        "controller": "task",
        "terrain_level": None,
        "restore_curriculum": False,
        "trace_path": None,
    }


def test_play_forwards_an_explicit_task_trace_path(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    from uerl.tasks.phantomx.config import PHANTOMX_TASK_ID
    from uerl.tasks.phantomx.evaluation import PhantomXEvaluationResult

    seen: dict[str, object] = {}

    def fake_run(config: object, **kwargs: object) -> PhantomXEvaluationResult:
        del config
        seen.update(kwargs)
        return PhantomXEvaluationResult(
            task_id=PHANTOMX_TASK_ID,
            checkpoint=Path("model.pt"),
            steps=1,
            completed_episodes=0,
            mean_forward_velocity=0.0,
            mean_speed_error=0.0,
            survival_rate=0.0,
            fall_rate=0.0,
            base_contact_rate=0.0,
            mean_episode_length=0.0,
            mean_command_vx=0.0,
            mean_command_speed=0.0,
            mean_actor_command_vx=0.0,
            mean_actor_command_speed=0.0,
            mean_command_observation_error=0.0,
            mean_action_clip_fraction=0.0,
            mean_torque_clip_fraction=0.0,
            mean_torque_over_limit=0.0,
            mean_reset_joint_error=0.0,
            mean_action_rate=0.0,
            mean_body_height=0.0,
            dominant_body_height_frequency_hz=0.0,
        )

    monkeypatch.setattr("uerl.training.run_evaluation", fake_run)
    checkpoint = _saved_checkpoint(tmp_path, PHANTOMX_TASK_ID)
    trace_path = tmp_path / "run" / "traces" / "task.yaml"

    play.main(
        [
            "--task",
            PHANTOMX_TASK_ID,
            "--checkpoint",
            str(checkpoint),
            "--trace",
            str(trace_path),
            "--controller",
            "fixed",
            "--fixed-velocity",
            "0.45,0,0",
        ]
    )

    assert seen["trace_path"] == trace_path
    assert seen["play_controller"] == "fixed"
    assert seen["fixed_velocity"] == (0.45, 0.0, 0.0)


def test_fixed_play_controller_requires_an_explicit_command(tmp_path: Path) -> None:
    checkpoint = _saved_checkpoint(tmp_path, "UERL-PhantomX-Walk-v0")

    with pytest.raises(SystemExit):
        play.main(
            [
                "--task",
                "UERL-PhantomX-Walk-v0",
                "--checkpoint",
                str(checkpoint),
                "--controller",
                "fixed",
            ]
        )


def test_train_and_export_do_not_accept_a_play_controller() -> None:
    assert "controller" not in {action.dest for action in train._parser()._actions}
    assert "controller" not in {action.dest for action in export._parser()._actions}


def test_cartpole_rejects_player_control_before_a_session() -> None:
    task = create_cartpole_task(create_cartpole_task_config())

    assert task._command_source.channels() == {}
    with pytest.raises(ConfigError, match="velocity command channel"):
        create_play_controller("player", task, 1)


def test_fixed_controller_holds_one_explicit_command_across_resets() -> None:
    task = PhantomXTask(PhantomXTaskConfig(), control_dt=0.02, batch_size=1, device="cpu")
    replacement = create_play_controller("fixed", task, 1, fixed_velocity=(0.45, 0.0, 0.0))
    assert isinstance(replacement, FixedVelocityController)
    task.use_command_source(replacement)

    before_reset = task.command_source.current()["velocity"]
    replacement.reset(torch.tensor([True]), {})
    after_reset = task.command_source.current()["velocity"]

    expected = torch.tensor([[0.45, 0.0, 0.0]])
    assert torch.equal(before_reset, expected)
    assert torch.equal(after_reset, expected)


def test_fixed_controller_requires_finite_velocity_width_three() -> None:
    task = PhantomXTask(PhantomXTaskConfig(), control_dt=0.02, batch_size=1, device="cpu")

    with pytest.raises(ConfigError, match="exactly three"):
        create_play_controller("fixed", task, 1, fixed_velocity=(0.45, 0.0))
    with pytest.raises(ConfigError, match="finite"):
        create_play_controller("fixed", task, 1, fixed_velocity=(float("nan"), 0.0, 0.0))


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


def test_custom_controller_publishes_its_velocity_into_the_policy_observation(monkeypatch: pytest.MonkeyPatch) -> None:
    from uerl.tasks import controllers

    monkeypatch.setattr(controllers, "_FACTORIES", dict(controllers._FACTORIES))
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


def test_custom_controller_width_mismatch_fails_at_assembly(monkeypatch: pytest.MonkeyPatch) -> None:
    from uerl.tasks import controllers

    monkeypatch.setattr(controllers, "_FACTORIES", dict(controllers._FACTORIES))
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

"""Verify the Catch scene through the real Worker and completed physics windows."""

from pathlib import Path

import torch

from tests.e2e.support.worker_runner import UE_CMD, UPROJECT
from uerl import UERLDirectEnv, UERLSession, UERLSessionAdapter
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.phantomx.catch import TARGET_CAPTURE_FIELD
from uerl.tasks.phantomx.pursuit import PHANTOMX_PURSUIT_TARGET_FIELD
from uerl.tasks.registry import create_default_registry
from uerl.training import build_launch_overrides, build_run_config


def test_catch_target_moves_contacts_robot_and_reset_clears_capture(tmp_path: Path) -> None:
    config = build_run_config("UERL-PhantomX-Catch-v0", overrides={
        **build_launch_overrides(ue_executable=Path(UE_CMD), project=Path(UPROJECT), map_name="/Engine/Maps/Entry"),
        "worker.decimation": "[4,4]",
        "worker.environment_config": (
            '{"environment.target_x_m": 0.6, "environment.target_y_m": 0.0, '
            '"environment.target_speed_mps": 0.3, "environment.target_path_radius_m": 0.3, '
            '"environment.use_player_target": 0.0}'
        ),
        "logging.run_directory": str(tmp_path / "catch-contact"),
    })
    task = create_default_registry().create_task(config.task_id, config.task)
    raw = UERLSession.open(config, process_controller=WorkerProcessController())
    env = None
    try:
        adapter = UERLSessionAdapter(raw)
        env = UERLDirectEnv(adapter, task, resolved_config=config)
        command = task.preprocess_actions(torch.zeros(1, 18), adapter.initial_state.values)
        first = adapter.step(command, step_decimation=4)
        assert first.state_valid.tolist() == [True]
        assert first.values[TARGET_CAPTURE_FIELD].item() == 0.0
        first_target = first.values[PHANTOMX_PURSUIT_TARGET_FIELD].clone()
        assert first_target[0, 1].item() > 0.0
        captured = False
        for _ in range(250):
            transition = adapter.step(command, step_decimation=4)
            assert transition.state_valid.tolist() == [True]
            assert transition.slot_fault_code.tolist() == [0]
            if transition.values[TARGET_CAPTURE_FIELD].item() == 1.0:
                captured = True
                break
        assert captured, "moving target did not produce a physical capture in the bounded rollout"
        env.reset()
        reset_transition = adapter.step(command, step_decimation=4)
        assert reset_transition.values[TARGET_CAPTURE_FIELD].item() == 0.0
        assert torch.allclose(
            reset_transition.values[PHANTOMX_PURSUIT_TARGET_FIELD], first_target,
            atol=1e-3, rtol=0.0,
        ), "reset must restore target position and its physical-time path phase"
    finally:
        if env is not None:
            env.close("catch_contact_complete")
        else:
            raw.close("catch_contact_setup_failed")

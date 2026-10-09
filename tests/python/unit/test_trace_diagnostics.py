"""Independent geometry, timing and reset oracles for offline reports."""

from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest
import yaml

from uerl.training.trace_diagnostics import diagnose_task_trace

P = "robot.body.base_link."


def _row(episode: int, step: int, *, dt: float = 0.02) -> dict[str, Any]:
    state = {
        P + "body_pose": [0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0],
        P + "body_linear_velocity": [0.0, 0.0, 0.0],
        P + "body_angular_velocity": [0.0, 0.0, 0.0],
    }
    return {
        "kind": "step",
        "episode_index": episode,
        "episode_step": step,
        "clocks": {"transition_dt_s": dt},
        "input": {
            "raw_state": deepcopy(state),
            "state_valid": True,
            "fault_code": 0,
            "commands": {"velocity": [0.4, 0.0, 0.0]},
        },
        "action": {"policy_action": [0.2, -0.4]},
        "transition": {
            "raw_state": deepcopy(state),
            "state_valid": True,
            "fault_code": 0,
            "reward": 2.0,
            "terminated": False,
            "truncated": False,
        },
    }


def _report(tmp_path: Path, rows: list[dict[str, Any]]) -> dict[str, Any]:
    path = tmp_path / "trace.yaml"
    path.write_text(
        yaml.safe_dump({"schema_version": 1, "source": {"side": "task"}, "status": "complete", "records": rows})
    )
    return diagnose_task_trace(path)


def test_rotated_body_uses_independent_velocity_and_time_weighted_rmse(tmp_path: Path) -> None:
    first = _row(0, 0, dt=0.01)
    second = _row(0, 1, dt=0.03)
    # +90 degree yaw maps world +Y to body +X. First interval tracks exactly.
    first["transition"]["raw_state"][P + "body_pose"][5:] = [2**-0.5, 2**-0.5]
    first["transition"]["raw_state"][P + "body_linear_velocity"] = [0.0, 0.4, 0.0]
    second["transition"]["raw_state"][P + "body_angular_velocity"] = [0.0, 0.0, 0.2]
    result = _report(tmp_path, [first, second])
    assert result["linear_velocity_rmse_m_s"] == pytest.approx(0.12**0.5)
    assert result["yaw_rate_rmse_rad_s"] == pytest.approx(0.03**0.5)
    assert result["measured_seconds"] == pytest.approx(0.04)
    assert result["action_rms"] == pytest.approx(0.1**0.5)
    assert result["within_episode_action_delta_rms"] == 0.0


def test_terminal_motion_excludes_reset_teleport_and_action_jump(tmp_path: Path) -> None:
    first, second = _row(0, 0), _row(1, 0)
    first["transition"]["raw_state"][P + "body_pose"][0:2] = [0.03, 0.04]
    first["transition"]["truncated"] = True
    first["reset"] = {"raw_state": {P + "body_pose": [100.0, 100.0, 0.18, 0.0, 0.0, 0.0, 1.0]}}
    second["input"]["raw_state"][P + "body_pose"][0:2] = [100.0, 100.0]
    second["transition"]["raw_state"][P + "body_pose"][0:2] = [100.0, 100.0]
    second["action"]["policy_action"] = [10.0, 20.0]
    result = _report(tmp_path, [first, second])
    assert result["planar_path_length_m"] == pytest.approx(0.05)
    assert result["episode_measured_displacement_m"] == {0: [0.03, 0.04], 1: [0.0, 0.0]}
    assert result["within_episode_action_delta_rms"] is None
    assert result["completed_episodes"] == 1
    assert result["reward_sum"] == 4.0
    assert result["success_rate"] is None
    assert result["reward_components"] is None


def test_missing_force_is_unavailable_but_recorded_zero_is_a_finding(tmp_path: Path) -> None:
    row = _row(0, 0)
    result = _report(tmp_path, [row])
    assert result["contact_force"] is None
    assert len(result["findings"]) == 1  # low motion only
    row["transition"]["raw_state"]["robot.body.foot.contact_force"] = [0.0]
    result = _report(tmp_path, [row])
    assert len(result["findings"]) == 2
    assert result["contact_force"]["robot.body.foot.contact_force"]["positive_samples"] == 0
    row["transition"]["raw_state"]["robot.body.foot.contact_force"] = [3.5]
    assert len(_report(tmp_path, [row])["findings"]) == 1


@pytest.mark.parametrize("dt", [0.0, -1.0, float("nan"), float("inf")])
def test_invalid_interval_is_rejected(tmp_path: Path, dt: float) -> None:
    with pytest.raises(ValueError, match="transition_dt_s"):
        _report(tmp_path, [_row(0, 0, dt=dt)])


def test_faulted_state_is_not_reported_as_valid_motion(tmp_path: Path) -> None:
    row = _row(0, 0)
    row["transition"]["fault_code"] = 3
    with pytest.raises(ValueError, match="transition: invalid or faulted"):
        _report(tmp_path, [row])


def test_zero_command_does_not_trigger_motion_warning(tmp_path: Path) -> None:
    row = _row(0, 0)
    row["input"]["commands"]["velocity"] = [0.0, 0.0, 0.0]
    assert _report(tmp_path, [row])["findings"] == []


def test_training_scalar_report_reads_real_events_with_separate_tag_windows(tmp_path: Path) -> None:
    from torch.utils.tensorboard import SummaryWriter

    from uerl.training.trace_diagnostics import summarize_training_scalars

    with SummaryWriter(str(tmp_path)) as writer:
        writer.add_scalar("Train/mean_reward", 8.0, 0)
        writer.add_scalar("Train/mean_reward", -2.0, 1)
        writer.add_scalar("Train/mean_reward", 4.0, 2)
        writer.add_scalar("Episode_Reward/track", 5.0, 2)
    result = summarize_training_scalars(tmp_path, last_iterations=2)
    scalars = result["scalars"]
    assert isinstance(scalars, dict)
    assert scalars["Train/mean_reward"] == {
        "latest_step": 2,
        "latest": 4.0,
        "window_first_step": 1,
        "window_samples": 2,
        "window_mean": 1.0,
    }
    assert scalars["Episode_Reward/track"]["window_mean"] == 5.0


def test_script_emits_yaml_and_preserves_input(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    from scripts.diagnose_training_trace import main

    _report(tmp_path, [_row(0, 0)])
    path = tmp_path / "trace.yaml"
    original = path.read_bytes()
    assert main([str(path)]) == 0
    payload = yaml.safe_load(capsys.readouterr().out)
    assert payload["steps"] == 1
    assert payload["linear_velocity_rmse_m_s"] == pytest.approx(0.4)
    assert path.read_bytes() == original
    path.write_text("records: [")
    assert main([str(path)]) == 2
    assert "Trace diagnosis failed" in capsys.readouterr().err

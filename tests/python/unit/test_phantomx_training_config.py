"""Verify the canonical PhantomX training configuration."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import replace
from pathlib import Path
from typing import cast

import pytest
import torch

from uerl.core.config.models import DirectTaskConfig
from uerl.tasks.phantomx import (
    PHANTOMX_DISCRETE_TERRAIN_ENVIRONMENT_ID,
    PHANTOMX_DISCRETE_TERRAIN_TASK_ID,
    PHANTOMX_FEET,
    PHANTOMX_SLOT_ISOLATED_ENVIRONMENT_ID,
    PHANTOMX_TERRAIN_ENVIRONMENT_ID,
    PHANTOMX_TERRAIN_TASK_ID,
    PhantomXTaskConfig,
    create_phantomx_discrete_terrain_registration,
    create_phantomx_discrete_terrain_worker_config,
    create_phantomx_pursuit_registration,
    create_phantomx_pursuit_worker_config,
    create_phantomx_registration,
    create_phantomx_terrain_registration,
    create_phantomx_terrain_worker_config,
    load_phantomx_training_config,
)
from uerl.tasks.phantomx.registration import create_phantomx_curriculum_manager
from uerl.training import build_run_config
from uerl.training.runner import run_training


def test_phantomx_uses_isaac_lab_physics_and_control_range() -> None:
    """Run PhantomX at 200 Hz with the configured inclusive frame range."""

    config = load_phantomx_training_config()

    assert config.worker.physics_dt == pytest.approx(0.005)
    assert config.worker.decimation == (1, 7)
    assert config.worker.physics_dt * config.worker.decimation[0] == pytest.approx(0.005)
    assert config.worker.physics_dt * config.worker.decimation[1] == pytest.approx(0.035)


def test_phantomx_50hz_training_uses_time_equivalent_runner_and_action_smoothing() -> None:
    """Preserve the validated 30 Hz physical horizons while controlling at 50 Hz."""

    config = load_phantomx_training_config()

    assert config.runner.rollout_length == 40
    assert config.runner.parameters["gamma"] == pytest.approx(0.99 ** (30 / 50))
    assert config.runner.parameters["lam"] == pytest.approx(0.95 ** (30 / 50))
    assert config.task.action_rate_weight == pytest.approx(2.4e-3)


def test_phantomx_preserves_episode_horizon_without_a_stall_gate() -> None:
    """Keep the 20 s timeout while letting slow terrain motion continue."""

    config = load_phantomx_training_config()
    control_dt_min = config.worker.physics_dt * config.worker.decimation[0]
    control_dt_max = config.worker.physics_dt * config.worker.decimation[1]

    assert config.task.max_episode_duration_s == pytest.approx(20.0)
    assert config.task.reference_dt_s == pytest.approx(0.02)
    assert control_dt_min == pytest.approx(0.005)
    assert control_dt_max == pytest.approx(0.035)
    assert not hasattr(config.task, "progress_grace_steps")
    assert not hasattr(config.task, "min_forward_progress_at_grace")


def test_phantomx_observation_contract_removes_base_contact_but_keeps_terrain_inputs() -> None:
    config = load_phantomx_training_config()
    observations = {(item.type.value, item.target) for item in config.robot_config.observations}

    assert ("contact", "base_link") not in observations
    assert {("contact", foot) for foot in PHANTOMX_FEET} <= observations
    assert {("contact_force", body) for body in ("base_link", *PHANTOMX_FEET)} <= observations
    assert {("terrain_height", "base_link"), ("ground_clearance", "base_link")} <= observations


def test_phantomx_command_speeds_cover_low_speed_without_exceeding_the_target() -> None:
    """Cover 0.05–0.5 m/s while keeping the existing maximum target."""

    command = load_phantomx_training_config().task.command

    assert (command.initial_speed_min, command.initial_speed_max) == pytest.approx((0.05, 0.5))
    assert (command.post_turn_speed_min, command.post_turn_speed_max) == pytest.approx((0.05, 0.5))


def test_phantomx_direct_task_defaults_to_the_20_second_horizon() -> None:
    assert PhantomXTaskConfig().max_episode_duration_s == pytest.approx(20.0)


def test_ac_py_unit_phantomx_002_training_uses_physical_time_ppo_objective() -> None:
    from uerl.tasks.cartpole import CARTPOLE_TASK_ID
    from uerl.training import build_rsl_rl_train_config

    phantomx = build_rsl_rl_train_config(build_run_config(PHANTOMX_TERRAIN_TASK_ID))
    cartpole = build_rsl_rl_train_config(build_run_config(CARTPOLE_TASK_ID))

    assert phantomx["algorithm"]["class_name"] == "uerl.training.rsl_rl.time_aware_ppo:TimeAwarePPO"
    assert phantomx["algorithm"]["reference_dt_s"] == pytest.approx(0.02)
    assert cartpole["algorithm"]["class_name"] == "rsl_rl.algorithms.ppo:PPO"
    assert "reference_dt_s" not in cartpole["algorithm"]


@pytest.mark.parametrize("objective", [None, "phantomx_physical_time_v1"])
def test_ac_py_unit_phantomx_003_old_objective_checkpoint_rejected_before_worker(
    tmp_path: Path, objective: str | None,
) -> None:
    """Old robust models cannot silently resume into the physical-time objective."""

    old_checkpoint = tmp_path / "old.pt"
    torch.save({"infos": {"uerl_curriculum": {}, "uerl_training_objective": objective}}, old_checkpoint)
    config = build_run_config(PHANTOMX_TERRAIN_TASK_ID)
    config = replace(config, runner=replace(config.runner, checkpoint=old_checkpoint))

    with pytest.raises(ValueError, match="same objective"):
        run_training(config)


def test_phantomx_direct_task_preserves_disabled_timeout() -> None:
    assert PhantomXTaskConfig.from_direct(DirectTaskConfig(max_episode_steps=0)).max_episode_duration_s is None


def test_phantomx_continuous_terrain_uses_full_episode_slot_terrain() -> None:
    config = build_run_config(PHANTOMX_TERRAIN_TASK_ID)

    assert config.worker.environment_id == PHANTOMX_TERRAIN_ENVIRONMENT_ID
    assert config.worker.slot_count == 64
    assert config.runner.rollout_length == 40
    assert config.worker.environment_config


def test_ac_py_unit_phantomx_001_terrain_definition_is_independent_of_slot_count() -> None:
    """The authored terrain profile is one Session config, not 64 robot configs."""

    one = build_run_config(PHANTOMX_TERRAIN_TASK_ID, overrides={"worker.slot_count": "1"})
    many = build_run_config(PHANTOMX_TERRAIN_TASK_ID, overrides={"worker.slot_count": "64"})

    assert one.worker.slot_count == 1
    assert many.worker.slot_count == 64
    assert one.worker.terrain_config_path == many.worker.terrain_config_path
    assert one.worker.terrain_config == many.worker.terrain_config


def test_phantomx_continuous_terrain_is_scaled_to_the_robot() -> None:
    terrain = create_phantomx_terrain_worker_config().terrain_config
    tiers = cast(Sequence[Mapping[str, object]], terrain["tiers"])
    tier_params = [cast(Mapping[str, object], tier["params"]) for tier in tiers]

    assert terrain["num_levels"] == len(tiers)
    assert terrain["cell_size"] == pytest.approx((30.0, 17.0))
    assert [tier["level"] for tier in tiers] == list(range(len(tiers)))
    assert [tier["primitive"] for tier in tiers] == ["heightfield"] * len(tiers)
    assert [set(params) for params in tier_params] == [
        {"noise_range", "noise_step", "horizontal_scale", "vertical_scale", "downsampled_scale"}
    ] * len(tiers)
    assert all(float(cast(float, tier["platform_width"])) > 0.0 for tier in tiers)
    assert all(
        float(cast(Sequence[float], params["noise_range"])[0]) < 0.0
        < float(cast(Sequence[float], params["noise_range"])[1])
        for params in tier_params
    )
    assert all(
        0.0 < float(cast(float, params["downsampled_scale"])) <= 1.0
        for params in tier_params
    )


def test_phantomx_discrete_terrain_uses_shared_atlas_and_random_grid() -> None:
    config = build_run_config(PHANTOMX_DISCRETE_TERRAIN_TASK_ID)
    terrain = create_phantomx_discrete_terrain_worker_config().terrain_config
    tiers = cast(Sequence[Mapping[str, object]], terrain["tiers"])
    tier_params = [cast(Mapping[str, object], tier["params"]) for tier in tiers]

    assert config.worker.environment_id == PHANTOMX_DISCRETE_TERRAIN_ENVIRONMENT_ID
    assert config.worker.slot_count == 64
    assert config.runner.parameters["run_name"] == "phantomx_discrete_terrain"
    assert set(config.worker.environment_config) == {
        "environment.spacing_x_m",
        "environment.spacing_y_m",
        "environment.columns",
        "environment.origin_x_m",
        "environment.origin_y_m",
        "environment.trace_start_z_m",
        "environment.trace_depth_m",
    }
    # SharedWorld enlarges the generated atlas without moving the 64 starts
    # toward its edge.  The source profile remains the compact 12 m terrain
    # used by the explicit Slot-isolated route.
    assert terrain["cell_size"] == pytest.approx((32.0, 32.0))
    assert terrain["border_width"] == pytest.approx(1.0)
    assert terrain["num_levels"] == len(tiers)
    assert [tier["primitive"] for tier in tiers] == ["boxes"] * len(tiers)
    assert [set(params) for params in tier_params] == [
        {
            "grid_width",
            "grid_height_range",
            "holes",
            "generator",
            "difficulty",
        }
    ] * len(tiers)
    assert all(float(cast(float, params["grid_width"])) > 0.0 for params in tier_params)
    assert all(params["holes"] is False and params["generator"] == "random_grid" for params in tier_params)
    assert all(
        float(cast(Sequence[float], params["grid_height_range"])[0])
        < float(cast(Sequence[float], params["grid_height_range"])[1])
        for params in tier_params
    )
    difficulties = [float(cast(float, params["difficulty"])) for params in tier_params]
    assert difficulties[0] == pytest.approx(0.0)
    assert difficulties[-1] == pytest.approx(1.0)
    assert difficulties == sorted(difficulties)


def test_phantomx_discrete_terrain_keeps_explicit_slot_isolated_route() -> None:
    config = create_phantomx_discrete_terrain_worker_config(isolated=True)

    assert config.environment_id == PHANTOMX_SLOT_ISOLATED_ENVIRONMENT_ID
    assert config.slot_count == 64
    assert config.environment_config == {
        "environment.spacing_x_m": pytest.approx(84.4),
        "environment.spacing_y_m": pytest.approx(14.4),
        "environment.columns": 8.0,
        "environment.origin_x_m": pytest.approx(-330.4),
        "environment.origin_y_m": pytest.approx(-50.4),
    }


def test_flat_phantomx_registration_remains_a_512_slot_shared_map() -> None:
    config = load_phantomx_training_config()

    assert config.worker.slot_count == 512
    assert config.worker.environment_id == "uerl.environment.shared_world"
    assert config.worker.terrain_config == {}


def test_phantomx_routes_select_expected_host_maps() -> None:
    flat = create_phantomx_registration()
    pursuit = create_phantomx_pursuit_registration()
    terrain = create_phantomx_terrain_registration()
    discrete = create_phantomx_discrete_terrain_registration()

    assert flat.session_config.map_path == "/Game/Maps/NewMap"
    assert pursuit.session_config.map_path == "/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo"
    assert pursuit.curriculum_factory is create_phantomx_curriculum_manager
    assert terrain.session_config.map_path == "/Engine/Maps/Entry"
    assert discrete.session_config.map_path == "/Engine/Maps/Entry"


def test_phantomx_pursuit_claims_one_authored_robot_without_procedural_terrain() -> None:
    worker = create_phantomx_pursuit_worker_config()

    assert worker.slot_count == 1
    assert worker.environment_id == "uerl.environment.authored_pursuit"
    assert worker.robot_config == {"robot.claim_authored_actor": 1.0}
    assert worker.terrain_config == {}

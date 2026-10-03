"""Validate curriculum, objective and runtime-policy recovery before UE startup."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest
import torch

from uerl.core.config import ResolvedRunConfig
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm
from uerl.errors import ConfigError
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.tasks.phantomx import PHANTOMX_TERRAIN_TASK_ID
from uerl.tasks.registry import create_default_registry
from uerl.training import build_run_config
from uerl.training.checkpoint import TrainingOptions, inspect_resume_checkpoint
from uerl.training.rsl_rl.runner import TRAINING_OPTIONS_KEY
from uerl.training.rsl_rl.time_aware_ppo import PHANTOMX_PHYSICAL_TIME_OBJECTIVE


def _checkpoint(tmp_path: Path, *, phantomx: bool = True) -> tuple[ResolvedRunConfig, dict[str, Any]]:
    config = build_run_config(
        PHANTOMX_TERRAIN_TASK_ID if phantomx else CARTPOLE_TASK_ID,
        overrides={
            "worker.slot_count": "2",
            "runner.device": "cpu",
            "runner.checkpoint": str(tmp_path / "model.pt"),
        },
    )
    manager = create_default_registry().create_curriculum(
        config.task_id,
        config.task,
        num_envs=2,
        device="cpu",
        run_seed=config.worker.run_seed,
    )
    curriculum = {} if manager is None else manager.state_dict()
    terrain = TerrainLevelTerm(num_levels=8 if phantomx else 1, num_envs=2, terrain_size_x=30.0)
    curriculum["terrain"] = dict(terrain.state_dict())
    payload: dict[str, Any] = {
        "actor_state_dict": {"weight": torch.zeros(1)},
        "critic_state_dict": {"weight": torch.zeros(1)},
        "optimizer_state_dict": {"state": {}, "param_groups": [{"params": [0], "lr": 0.001}]},
        "iter": 12,
        "infos": {
            "uerl_curriculum": curriculum,
            "uerl_direct_env_random": {"step_decimation_generator": torch.Generator().get_state()},
        },
    }
    if phantomx:
        payload["infos"]["uerl_training_objective"] = PHANTOMX_PHYSICAL_TIME_OBJECTIVE
    return config, payload


def _save(config: ResolvedRunConfig, payload: dict[str, Any]) -> None:
    assert config.runner.checkpoint is not None
    torch.save(payload, config.runner.checkpoint)


def test_complete_legacy_cartpole_checkpoint_needs_no_recovery_flags(tmp_path: Path) -> None:
    config, payload = _checkpoint(tmp_path, phantomx=False)
    _save(config, payload)
    state = inspect_resume_checkpoint(config)
    assert state.options == TrainingOptions()
    assert state.iteration == 12
    assert state.curriculum_terms == ("terrain",)
    assert state.restores_decimation_rng


def test_legacy_phantomx_requires_explicit_normalization_and_fixed_terrain(tmp_path: Path) -> None:
    config, payload = _checkpoint(tmp_path)
    _save(config, payload)
    with pytest.raises(ConfigError, match="resume-normalization"):
        inspect_resume_checkpoint(config)
    state = inspect_resume_checkpoint(config, normalization="frozen")
    assert state.options == TrainingOptions(freeze_observation_normalization=True)
    with pytest.raises(ConfigError, match="adaptive terrain"):
        inspect_resume_checkpoint(config, terrain_level=2, normalization="update")
    del payload["infos"]["uerl_curriculum"]["terrain"]
    _save(config, payload)
    with pytest.raises(ConfigError, match="explicit --terrain-level"):
        inspect_resume_checkpoint(config, normalization="update")
    state = inspect_resume_checkpoint(config, terrain_level=2, normalization="update")
    assert state.options == TrainingOptions(terrain_level=2)
    assert state.curriculum_terms == ("command",)


def test_new_checkpoint_recovers_fixed_level_and_frozen_normalization(tmp_path: Path) -> None:
    config, payload = _checkpoint(tmp_path)
    del payload["infos"]["uerl_curriculum"]["terrain"]
    payload["infos"][TRAINING_OPTIONS_KEY] = TrainingOptions(2, True).to_dict()
    _save(config, payload)
    state = inspect_resume_checkpoint(config)
    assert state.options == TrainingOptions(2, True)
    assert state.options_source == "checkpoint"
    with pytest.raises(ConfigError, match="differs from the recorded"):
        inspect_resume_checkpoint(config, terrain_level=3)
    with pytest.raises(ConfigError, match="differs from the recorded"):
        inspect_resume_checkpoint(config, normalization="update")


@pytest.mark.parametrize(
    "missing", ["optimizer_state_dict", "iter", "uerl_training_objective", "uerl_direct_env_random", "uerl_curriculum"]
)
def test_incomplete_training_state_is_rejected_before_session(tmp_path: Path, missing: str) -> None:
    config, payload = _checkpoint(tmp_path)
    payload["infos"][TRAINING_OPTIONS_KEY] = TrainingOptions().to_dict()
    if missing.startswith("uerl_"):
        del payload["infos"][missing]
    else:
        del payload[missing]
    _save(config, payload)
    with pytest.raises(ConfigError):
        inspect_resume_checkpoint(config)


@pytest.mark.parametrize("invalid", ["actor", "optimizer", "random", "terrain"])
def test_malformed_saved_state_is_rejected(tmp_path: Path, invalid: str) -> None:
    config, payload = _checkpoint(tmp_path)
    payload["infos"][TRAINING_OPTIONS_KEY] = TrainingOptions().to_dict()
    if invalid == "actor":
        payload["actor_state_dict"] = {}
    elif invalid == "optimizer":
        payload["optimizer_state_dict"] = {}
    elif invalid == "random":
        payload["infos"]["uerl_direct_env_random"]["step_decimation_generator"] = torch.zeros(1)
    else:
        payload["infos"]["uerl_curriculum"]["terrain"]["levels"] = [0]
    _save(config, payload)
    with pytest.raises(ConfigError):
        inspect_resume_checkpoint(config)


@pytest.mark.parametrize("interruption", [KeyboardInterrupt, SystemExit])
def test_checkpoint_read_preserves_process_interruptions(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    interruption: type[BaseException],
) -> None:
    from typing import NoReturn

    config, payload = _checkpoint(tmp_path, phantomx=False)
    _save(config, payload)

    def interrupted(*args: object, **kwargs: object) -> NoReturn:
        raise interruption

    monkeypatch.setattr(torch, "load", interrupted)
    with pytest.raises(interruption):
        inspect_resume_checkpoint(config)

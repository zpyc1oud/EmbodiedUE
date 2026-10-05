"""Inspect continuation state before starting UE; loading remains owned by the runner."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import NoReturn, cast

import torch

from ..core.config import ResolvedRunConfig
from ..core.direct.curriculum import CurriculumManager
from ..core.mdp.lib.curriculum import TerrainLevelTerm
from ..errors import ConfigError
from ..tasks.registry import create_default_registry
from .rsl_rl.runner import TRAINING_OPTIONS_KEY
from .rsl_rl.time_aware_ppo import PHANTOMX_PHYSICAL_TIME_OBJECTIVE


@dataclass(frozen=True, slots=True)
class TrainingOptions:
    """Training semantics previously held only in Python call arguments."""

    terrain_level: int | None = None
    freeze_observation_normalization: bool = False

    def to_dict(self) -> dict[str, object]:
        return {
            "terrain_level": self.terrain_level,
            "freeze_observation_normalization": self.freeze_observation_normalization,
        }


@dataclass(frozen=True, slots=True)
class ResumeState:
    options: TrainingOptions
    iteration: int
    curriculum_terms: tuple[str, ...]
    restores_decimation_rng: bool
    options_source: str


def inspect_resume_checkpoint(
    config: ResolvedRunConfig,
    *,
    terrain_level: int | None = None,
    normalization: str | None = None,
) -> ResumeState:
    """Validate saved state and recover runtime options, including explicit legacy input."""

    if normalization not in (None, "update", "frozen"):
        _fail("expected update or frozen", "normalization")
    checkpoint = config.runner.checkpoint
    if checkpoint is None or not checkpoint.is_file():
        raise ConfigError("continuation checkpoint not found", code="INVALID_CHECKPOINT", path=str(checkpoint))
    try:
        payload = torch.load(checkpoint, map_location="cpu", weights_only=False)
    except Exception as exc:
        # Scope the conversion to file reading/deserialization, not validation.
        # Exception deliberately excludes KeyboardInterrupt and SystemExit.
        raise ConfigError(
            "cannot read checkpoint; restore a complete trusted checkpoint "
            "(fetch the actual Git LFS content if needed) and retry",
            code="INVALID_CHECKPOINT",
            path=str(checkpoint),
        ) from exc
    if not isinstance(payload, Mapping):
        _fail("expected a checkpoint mapping", "checkpoint")
    for key in ("actor_state_dict", "critic_state_dict", "optimizer_state_dict"):
        if not isinstance(payload.get(key), Mapping):
            _fail("missing training state", key)
    for key in ("actor_state_dict", "critic_state_dict"):
        if not payload[key]:
            _fail("model state is empty", key)
    optimizer = payload["optimizer_state_dict"]
    if not isinstance(optimizer.get("state"), Mapping) or not isinstance(optimizer.get("param_groups"), list):
        _fail("optimizer state requires state and param_groups", "optimizer_state_dict")
    if not optimizer["param_groups"]:
        _fail("optimizer has no parameter groups", "optimizer_state_dict")
    iteration = payload.get("iter")
    if type(iteration) is not int or iteration < 0:
        _fail("missing non-negative iteration", "iter")
    raw_infos = payload.get("infos")
    infos = {} if raw_infos is None else raw_infos
    if not isinstance(infos, Mapping):
        _fail("expected a mapping", "infos")
    if (
        config.task.reference_dt_s is not None
        and infos.get("uerl_training_objective") != PHANTOMX_PHYSICAL_TIME_OBJECTIVE
    ):
        _fail(
            "PhantomX physical-time training requires a checkpoint from the same objective", "uerl_training_objective"
        )
    curriculum = infos.get("uerl_curriculum", {})
    if not isinstance(curriculum, Mapping):
        _fail("expected a mapping", "uerl_curriculum")
    saved_options = infos.get(TRAINING_OPTIONS_KEY)
    if saved_options is not None:
        if not isinstance(saved_options, Mapping) or set(saved_options) != {
            "terrain_level",
            "freeze_observation_normalization",
        }:
            _fail("invalid training options", TRAINING_OPTIONS_KEY)
        level = saved_options["terrain_level"]
        frozen = saved_options["freeze_observation_normalization"]
        if level is not None and (type(level) is not int or level < 0):
            _fail("expected a non-negative terrain level or null", "terrain_level")
        if type(frozen) is not bool:
            _fail("expected a boolean", "freeze_observation_normalization")
        if terrain_level is not None and terrain_level != level:
            _fail("--terrain-level differs from the recorded training mode", "terrain_level")
        if normalization is not None and (normalization == "frozen") != frozen:
            _fail("--resume-normalization differs from the recorded policy", "freeze_observation_normalization")
        options_source = "checkpoint"
    else:
        # Adaptive terrain is identifiable by its saved term. A fixed level is
        # not: command.txt is not a portable authoritative configuration file.
        if "terrain" in curriculum:
            if terrain_level is not None:
                _fail("adaptive terrain checkpoint cannot continue as fixed terrain", "terrain_level")
            level = None
        elif config.worker.terrain_config:
            if terrain_level is None:
                _fail("old fixed-terrain Run requires explicit --terrain-level", "terrain_level")
            level = terrain_level
        else:
            level = terrain_level
        if config.runner.parameters.get("obs_normalization") and normalization is None:
            _fail("old Run requires --resume-normalization update or frozen", "freeze_observation_normalization")
        frozen = normalization == "frozen"
        options_source = "legacy state and explicit recovery inputs"
    options = TrainingOptions(cast(int | None, level), frozen)
    _validate_curriculum(config, options, curriculum)
    random_state = infos.get("uerl_direct_env_random")
    if random_state is None:
        if config.worker.decimation[0] != config.worker.decimation[1]:
            _fail("variable-decimation continuation requires saved generator state", "uerl_direct_env_random")
    else:
        if not isinstance(random_state, Mapping) or set(random_state) != {"step_decimation_generator"}:
            _fail("invalid decimation random state", "uerl_direct_env_random")
        state = random_state["step_decimation_generator"]
        if not isinstance(state, torch.Tensor) or state.dtype != torch.uint8:
            _fail("expected uint8 generator state", "uerl_direct_env_random")
        try:
            torch.Generator(device="cpu").set_state(state.cpu())
        except RuntimeError as exc:
            _fail(str(exc), "uerl_direct_env_random")
    if config.runner.parameters.get("rnd_cfg") is not None:
        for key in ("rnd_state_dict", "rnd_optimizer_state_dict"):
            if not isinstance(payload.get(key), Mapping):
                _fail("missing RND training state", key)
    return ResumeState(options, iteration, tuple(sorted(curriculum)), random_state is not None, options_source)


def _validate_curriculum(
    config: ResolvedRunConfig,
    options: TrainingOptions,
    state: Mapping[str, object],
) -> None:
    manager = create_default_registry().create_curriculum(
        config.task_id,
        config.task,
        num_envs=config.worker.slot_count,
        device="cpu",
        run_seed=config.worker.run_seed,
    )
    terrain = config.worker.terrain_config
    if options.terrain_level is not None:
        if not terrain or not 0 <= options.terrain_level < cast(int, terrain["num_levels"]):
            _fail("fixed terrain level is outside the recorded terrain", "terrain_level")
    elif terrain:
        term = TerrainLevelTerm(
            num_levels=cast(int, terrain["num_levels"]),
            num_envs=config.worker.slot_count,
            terrain_size_x=float(cast(Sequence[float], terrain["cell_size"])[0]),
            device="cpu",
            seed=config.worker.run_seed,
        )
        if manager is None:
            manager = CurriculumManager({"terrain": term})
        else:
            if "terrain" in manager.terms:
                _fail("Task already registers terrain curriculum", "uerl_curriculum")
            manager = CurriculumManager({**manager.terms, "terrain": term})
    if manager is None:
        if state:
            _fail("checkpoint contains curriculum but this Task has none", "uerl_curriculum")
    else:
        try:
            manager.load_state_dict(state)
        except (ValueError, TypeError, KeyError) as exc:
            _fail(str(exc), "uerl_curriculum")


def _fail(message: str, path: str) -> NoReturn:
    raise ConfigError(message, code="INVALID_CHECKPOINT", path=path)

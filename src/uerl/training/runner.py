"""Assemble and run the task-neutral RSL-RL training workflow."""

from __future__ import annotations

import json
import re
import socket
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from ..core.config import PresentationMode, ResolvedRunConfig, RunConfigResolver
from ..core.config.robot import RobotSpec
from ..core.direct.curriculum import CurriculumManager
from ..core.direct.env import UERLDirectEnv
from ..core.direct.profiling import StageProfiler
from ..core.direct.task import DirectTask
from ..core.mdp.lib.curriculum import TerrainLevelTerm
from ..core.mdp.managers.event import EventManager
from ..runtime.session import UERLSession, UERLSessionAdapter, WorkerProcessController
from ..tasks.evaluation import EvaluationSummary, TaskEvaluator
from ..tasks.registry import create_default_registry
from .rsl_rl import UERLOnPolicyRunner, UERLVecEnvWrapper
from .rsl_rl.time_aware_ppo import PHANTOMX_PHYSICAL_TIME_OBJECTIVE

_CONFIG_PATH = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)+$")
DEFAULT_TRAINING_MAP = "/Game/Maps/NewMap"
# Adaptive Runs register the terrain term under this name; fixed-level Runs omit it.
_TERRAIN_CURRICULUM_TERM = "terrain"
# Keep UE/Session state and Task mathematics on CPU. RSL-RL receives this
# environment device separately and moves observations/actions to its runner
# device for model inference and PPO updates.
ENVIRONMENT_DEVICE = "cpu"
# Game DefaultEngine.ini uses deploy substepping. A Worker process must lock
# the Chaos scene to one integration step per engine frame before the scene is created.
WORKER_LOCKSTEP_PHYSICS_ARGS: tuple[str, ...] = (
    "-ini:Engine:[/Script/Engine.PhysicsSettings]:bTickPhysicsAsync=False",
    "-ini:Engine:[/Script/Engine.PhysicsSettings]:bSubstepping=False",
    "-ini:Engine:[/Script/Engine.PhysicsSettings]:bSubsteppingAsync=False",
)


@dataclass(frozen=True, slots=True)
class TrainingResult:
    """Record the durable outputs of one completed training Run."""

    task_id: str
    run_directory: Path
    checkpoint: Path
    metrics_directory: Path
    normalized_hash: str
    iterations: int


def parse_config_options(entries: Sequence[str]) -> dict[str, str]:
    """Parse repeated ``--dotted.path value`` entries into Resolver input.

    Args:
        entries: Unknown command-line tokens left after fixed entry arguments
            have been parsed.

    Returns:
        A mapping of unique configuration paths to their raw string values.

    Raises:
        ValueError: If an entry does not use the direct path/value syntax, a
            value is missing, or a path occurs more than once.
    """

    options: dict[str, str] = {}
    index = 0
    while index < len(entries):
        token = entries[index]
        if not token.startswith("--") or token == "--":
            raise ValueError(f"expected --<dotted-path> <value>, got {token!r}")
        path = token[2:]
        if "=" in path or not _CONFIG_PATH.fullmatch(path):
            raise ValueError(f"expected --<dotted-path> <value>, got {token!r}")
        if path in options:
            raise ValueError(f"duplicate configuration path: {path}")
        if index + 1 >= len(entries) or entries[index + 1].startswith("--"):
            raise ValueError(f"missing value for configuration path: {path}")
        options[path] = entries[index + 1]
        index += 2
    return options


def build_launch_overrides(
    *,
    ue_executable: Path,
    project: Path,
    map_name: str,
    port: int | None = None,
    presentation: PresentationMode = PresentationMode.NONE,
    window_size: tuple[int, int] = (640, 360),
) -> dict[str, str]:
    """Build generic Session launch values for one UE Worker Run.

    Args:
        ue_executable: Path to ``UnrealEditor-Cmd.exe``.
        project: UE project containing the UERL plugin.
        map_name: UE map argument passed to the Worker.
        port: Bridge port; a free local port is reserved when omitted.
        presentation: Worker presentation mode. ``NONE`` launches a headless
            Null-RHI Worker; ``VIEWPORT`` installs the UERL observer; ``GAMEPLAY``
            preserves the map's PlayerController and Pawn in a visible window.
        window_size: ``(width, height)`` used only in viewport mode.

    Returns:
        A mapping of Session config paths to their raw override values. The UE
        command line derives entirely from ``session.worker_args``; the UE side
        validates presentation from ``-uerlpresentation=`` and rejects
        ``-nullrhi`` in viewport mode, so the two modes emit disjoint RHI flags.
    """

    bridge_port = port if port is not None else _free_port()
    worker_args = [
        str(project),
        map_name,
        "-game",
        f"-uerlport={bridge_port}",
        f"-uerlpresentation={presentation.value}",
        *_presentation_args(presentation, window_size),
        "-nopause",
        "-nosplash",
        "-stdout",
        "-FullStdOutLogOutput",
        *WORKER_LOCKSTEP_PHYSICS_ARGS,
    ]
    return {
        "session.worker_executable": str(ue_executable),
        "session.worker_args": json.dumps(worker_args),
        "session.map_path": map_name,
        "session.port": str(bridge_port),
        "session.presentation_mode": presentation.value,
    }


def _presentation_args(
    presentation: PresentationMode,
    window_size: tuple[int, int],
) -> list[str]:
    """Return the RHI and windowing flags required by one presentation mode."""

    if presentation is PresentationMode.NONE:
        return ["-nullrhi", "-unattended", "-nosound"]
    width, height = window_size
    if presentation is PresentationMode.GAMEPLAY:
        return [
            "-windowed",
            f"-ResX={width}",
            f"-ResY={height}",
            "-nosound",
        ]
    return [
        "-windowed",
        f"-ResX={width}",
        f"-ResY={height}",
        "-nosound",
        # Viewport training follows the first generic robot instead of using a fixed shot.
        "-uerlfollowrobot=1",
    ]


def build_run_config(
    task_id: str,
    *,
    overrides: Mapping[str, str] | None = None,
) -> ResolvedRunConfig:
    """Resolve one registered Task through the shared Config boundary."""

    return RunConfigResolver(create_default_registry()).resolve(task_id, overrides)


def build_rsl_rl_train_config(config: ResolvedRunConfig) -> dict[str, Any]:
    """Translate typed Python runner settings into one fresh RSL-RL config."""

    parameters = cast(Mapping[str, Any], config.runner.parameters)
    raw_hidden_dims = parameters.get("hidden_dims", (32, 32))
    hidden_dims = tuple(cast(Sequence[int], raw_hidden_dims))
    task_name = _task_name(config.task_id)
    raw_obs_groups = cast(
        Mapping[str, Sequence[str]],
        parameters.get("obs_groups", {"actor": ["policy"], "critic": ["policy"]}),
    )
    obs_groups = {name: list(values) for name, values in raw_obs_groups.items()}
    train_config: dict[str, Any] = {
        "seed": config.worker.run_seed,
        "num_steps_per_env": config.runner.rollout_length,
        "save_interval": int(parameters.get("save_interval", 50)),
        "experiment_name": str(parameters.get("experiment_name", task_name)),
        "run_name": str(parameters.get("run_name", task_name)),
        "obs_groups": obs_groups,
        "actor": {
            "class_name": str(parameters.get("actor_class_name", "rsl_rl.models.mlp_model:MLPModel")),
            "hidden_dims": hidden_dims,
            "activation": str(parameters.get("activation", "elu")),
            "obs_normalization": bool(parameters.get("obs_normalization", False)),
            "distribution_cfg": {
                "class_name": "rsl_rl.modules.distribution:GaussianDistribution",
                "init_std": float(parameters.get("init_std", 1.0)),
            },
        },
        "critic": {
            "class_name": str(parameters.get("critic_class_name", "rsl_rl.models.mlp_model:MLPModel")),
            "hidden_dims": hidden_dims,
            "activation": str(parameters.get("activation", "elu")),
            "obs_normalization": bool(parameters.get("obs_normalization", False)),
        },
        "algorithm": {
            "class_name": str(parameters.get("algorithm_class_name", "rsl_rl.algorithms.ppo:PPO")),
            "num_learning_epochs": int(parameters.get("num_learning_epochs", 5)),
            "num_mini_batches": int(parameters.get("num_mini_batches", 4)),
            "clip_param": float(parameters.get("clip_param", 0.2)),
            "gamma": float(parameters.get("gamma", 0.99)),
            "lam": float(parameters.get("lam", 0.95)),
            "learning_rate": float(parameters.get("learning_rate", 1.0e-3)),
            "entropy_coef": float(parameters.get("entropy_coef", 0.01)),
            "rnd_cfg": parameters.get("rnd_cfg"),
            "symmetry_cfg": parameters.get("symmetry_cfg"),
        },
        "check_for_nan": bool(parameters.get("check_for_nan", True)),
    }
    if config.task.reference_dt_s is not None:
        train_config["algorithm"]["reference_dt_s"] = config.task.reference_dt_s
    return train_config


def run_training(
    config: ResolvedRunConfig,
    *,
    terrain_level: int | None = None,
    event_manager_factory: Callable[[RobotSpec], EventManager] | None = None,
    restore_curriculum: bool = True,
    freeze_observation_normalization: bool = False,
) -> TrainingResult:
    """Run the resolved Task and close all acquired resources on every exit path.

    Args:
        config: Resolved immutable Run configuration.
        terrain_level: Optional zero-based terrain level fixed for every Slot.
            When set, automatic terrain promotion/demotion is disabled.
        freeze_observation_normalization: Keep checkpoint-loaded actor and
            critic observation statistics fixed during warm-start training.
    """

    import torch
    if config.runner.max_iterations < 1:
        raise ValueError("config.runner.max_iterations must be positive")
    torch.manual_seed(config.worker.run_seed)

    registry = create_default_registry()
    task = registry.create_task(config.task_id, config.task)
    enable_noise = getattr(task, "enable_observation_corruption", None)
    if callable(enable_noise):
        noise_generator = torch.Generator()
        noise_generator.manual_seed(config.worker.run_seed)
        enable_noise(noise_generator)
    if event_manager_factory is None:
        registered_events = registry.resolve(config.task_id).event_manager_factory
        if registered_events is not None:

            def event_manager_factory(spec: RobotSpec) -> EventManager:
                return registered_events(
                    config.task,
                    spec,
                    batch_size=config.worker.slot_count,
                    device=ENVIRONMENT_DEVICE,
                    run_seed=config.worker.run_seed,
                )

    resume_checkpoint = config.runner.checkpoint
    if (
        resume_checkpoint is not None
        and resume_checkpoint.is_file()
        and config.task.reference_dt_s is not None
    ):
        checkpoint_info = torch.load(resume_checkpoint, map_location="cpu", weights_only=False).get("infos")
        if (
            not isinstance(checkpoint_info, Mapping)
            or checkpoint_info.get("uerl_training_objective") != PHANTOMX_PHYSICAL_TIME_OBJECTIVE
        ):
            raise ValueError(
                "PhantomX physical-time training requires a checkpoint from the same objective; "
                "start a new Run for pre-change checkpoints"
            )
    checkpoint_path = config.logging.run_directory / "model_final.pt"
    if freeze_observation_normalization and (
        resume_checkpoint is None or not resume_checkpoint.is_file()
    ):
        raise FileNotFoundError(
            "freeze_observation_normalization requires an existing checkpoint: "
            f"{resume_checkpoint or checkpoint_path}"
        )
    curriculum_manager = registry.create_curriculum(
        config.task_id,
        config.task,
        num_envs=config.worker.slot_count,
        device=ENVIRONMENT_DEVICE,
        run_seed=config.worker.run_seed,
    )
    raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
    metrics_directory = config.logging.run_directory / "rsl_rl"
    direct_env: UERLDirectEnv | None = None
    vec_env: UERLVecEnvWrapper | None = None
    try:
        stage_profiler = StageProfiler(
            rollout_length=config.runner.rollout_length,
            jsonl_path=config.logging.run_directory / "stage_latency.jsonl",
        )
        if terrain_level is None:
            curriculum_manager = _attach_terrain_curriculum(
                config,
                task,
                curriculum_manager,
            )
        direct_env = UERLDirectEnv(
            UERLSessionAdapter(raw_session, device=ENVIRONMENT_DEVICE),
            task,
            resolved_config=config,
            device=ENVIRONMENT_DEVICE,
            profiler=stage_profiler,
            curriculum_manager=curriculum_manager,
            event_manager_factory=event_manager_factory,
            initial_terrain_level=terrain_level,
        )
        train_config = build_rsl_rl_train_config(config)
        vec_env = UERLVecEnvWrapper(direct_env, cfg=config)
        runner = UERLOnPolicyRunner(
            vec_env,
            train_config,
            log_dir=str(metrics_directory),
            device=config.runner.device,
        )
        if resume_checkpoint is not None and resume_checkpoint.is_file():
            runner.load(
                str(resume_checkpoint),
                map_location=config.runner.device,
                restore_curriculum=restore_curriculum,
                skip_curriculum_terms=() if terrain_level is None else (_TERRAIN_CURRICULUM_TERM,),
            )
            if freeze_observation_normalization:
                runner.freeze_observation_normalization()
        runner.learn(config.runner.max_iterations, init_at_random_ep_len=True)
        checkpoint_path.parent.mkdir(parents=True, exist_ok=True)
        runner.save(str(checkpoint_path))
    finally:
        if vec_env is not None:
            vec_env.close("training_complete")
        elif direct_env is not None:
            direct_env.close("training_setup_failed")
        else:
            raw_session.close("training_setup_failed")

    return TrainingResult(
        task_id=config.task_id,
        run_directory=config.logging.run_directory,
        checkpoint=checkpoint_path,
        metrics_directory=metrics_directory,
        normalized_hash=config.normalized_hash,
        iterations=config.runner.max_iterations,
    )


def run_evaluation(
    config: ResolvedRunConfig,
    *,
    checkpoint: Path,
    steps: int,
    terrain_level: int | None = None,
    evaluator_factory: Callable[[float], TaskEvaluator | None] | None = None,
    restore_curriculum: bool = True,
    play_controller: str = "task",
) -> object:
    """Run a checkpoint deterministically and aggregate fixed-horizon metrics.

    ``terrain_level`` fixes the procedural terrain tier for playback; leaving
    it unset preserves the task's adaptive curriculum.
    """

    import torch
    if steps < 1:
        raise ValueError("steps must be positive")
    if not checkpoint.is_file():
        raise FileNotFoundError(checkpoint)
    torch.manual_seed(config.worker.run_seed)

    registry = create_default_registry()
    task = registry.create_task(config.task_id, config.task)
    task.align_batch(config.worker.slot_count)
    player_controller = None
    if play_controller != "task":
        from ..tasks.controllers import PlayerVelocityController, create_play_controller

        replacement = create_play_controller(play_controller, task, config.worker.slot_count)
        if replacement is not None:
            task.use_command_source(replacement)
            if isinstance(replacement, PlayerVelocityController):
                player_controller = replacement
    curriculum_manager = registry.create_curriculum(
        config.task_id,
        config.task,
        num_envs=config.worker.slot_count,
        device=ENVIRONMENT_DEVICE,
        run_seed=config.worker.run_seed,
    )
    if terrain_level is None:
        curriculum_manager = _attach_terrain_curriculum(
            config,
            task,
            curriculum_manager,
        )
    else:
        # A requested playback tier is fixed for this evaluation.  A training
        # checkpoint may also contain adaptive command/terrain state; restoring
        # it into the fixed-tier manager would either change the requested tier
        # or fail when the terrain term is intentionally absent.
        restore_curriculum = False
    raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
    direct_env: UERLDirectEnv | None = None
    vec_env: UERLVecEnvWrapper | None = None
    try:
        direct_env = UERLDirectEnv(
            UERLSessionAdapter(raw_session, device=ENVIRONMENT_DEVICE),
            task,
            resolved_config=config,
            device=ENVIRONMENT_DEVICE,
            curriculum_manager=curriculum_manager,
            initial_terrain_level=terrain_level,
        )
        vec_env = UERLVecEnvWrapper(direct_env, cfg=config)
        runner = UERLOnPolicyRunner(
            vec_env,
            build_rsl_rl_train_config(config),
            log_dir=None,
            device=config.runner.device,
        )
        if restore_curriculum:
            runner.load(str(checkpoint), map_location=config.runner.device)
        else:
            runner.load(
                str(checkpoint),
                map_location=config.runner.device,
                restore_curriculum=False,
            )
        policy = runner.get_inference_policy(device=config.runner.device)
        observations = vec_env.get_observations().to(config.runner.device)

        sample_hz = 1.0 / (config.worker.physics_dt * config.worker.decimation[1])
        evaluator = (
            evaluator_factory(sample_hz)
            if evaluator_factory is not None
            else registry.create_evaluator(config.task_id, sample_hz=sample_hz)
        )
        completed_episodes = 0
        episode_length_sum = 0
        reward_sum = 0.0
        with torch.inference_mode():
            for _ in range(steps):
                actions = policy(observations)
                if player_controller is not None:
                    actions = player_controller.gate_actions(actions)
                previous_observations = observations
                observations, rewards, dones, extras = vec_env.step(actions.to(vec_env.device))
                observations = observations.to(config.runner.device)
                metrics = {**extras["log"], "transition_dt_s": extras["transition_dt"]}
                if evaluator is not None:
                    evaluator.observe(
                        observations=previous_observations.to(vec_env.device),
                        actions=actions.to(vec_env.device),
                        metrics=metrics,
                        rewards=rewards,
                        dones=dones,
                        terminal_episode_lengths=extras["terminal_episode_length"],
                    )
                completed_episodes += int(dones.sum().item())
                reward_sum += float(rewards.mean().item())
                episode_length_sum += int(extras["terminal_episode_length"][dones].sum().item())
    finally:
        if vec_env is not None:
            vec_env.close("evaluation_complete")
        elif direct_env is not None:
            direct_env.close("evaluation_setup_failed")
        else:
            raw_session.close("evaluation_setup_failed")

    summary = EvaluationSummary(
        task_id=config.task_id,
        checkpoint=checkpoint,
        steps=steps,
        completed_episodes=completed_episodes,
        mean_episode_length=episode_length_sum / completed_episodes if completed_episodes else 0.0,
        mean_reward=reward_sum / steps,
    )
    return evaluator.finish(summary) if evaluator is not None else summary


def _task_name(task_id: str) -> str:
    """Derive a stable generic RSL-RL name from a registered Task ID."""

    return re.sub(r"[^A-Za-z0-9]+", "_", task_id).strip("_").lower()


def _attach_terrain_curriculum(
    config: ResolvedRunConfig,
    task: DirectTask,
    curriculum_manager: CurriculumManager | None,
) -> CurriculumManager | None:
    """Add the registered terrain term to a normal adaptive Run.

    Training and evaluation must expose the same named curriculum terms before
    a checkpoint's curriculum state is restored.  Fixed-level playback omits
    this helper so the requested terrain tier remains unchanged.
    """

    terrain_config = config.worker.terrain_config
    if not terrain_config:
        return curriculum_manager
    terrain_term = TerrainLevelTerm(
        num_levels=cast(int, terrain_config["num_levels"]),
        num_envs=config.worker.slot_count,
        terrain_size_x=float(cast(Sequence[float], terrain_config["cell_size"])[0]),
        device=ENVIRONMENT_DEVICE,
        seed=config.worker.run_seed,
    )
    if curriculum_manager is None:
        return CurriculumManager({_TERRAIN_CURRICULUM_TERM: terrain_term})
    if _TERRAIN_CURRICULUM_TERM in curriculum_manager.terms:
        raise ValueError("curriculum manager already registers a terrain term")
    return CurriculumManager({**curriculum_manager.terms, _TERRAIN_CURRICULUM_TERM: terrain_term})


def _free_port() -> int:
    """Reserve one local TCP port for a launched UE Worker."""

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


__all__ = [
    "TrainingResult",
    "WORKER_LOCKSTEP_PHYSICS_ARGS",
    "build_launch_overrides",
    "build_rsl_rl_train_config",
    "build_run_config",
    "parse_config_options",
    "run_evaluation",
    "run_training",
]

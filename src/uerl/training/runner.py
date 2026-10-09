"""Assemble and run the task-neutral RSL-RL training workflow."""

from __future__ import annotations

import re
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

from ..core.config import ResolvedRunConfig
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
from .checkpoint import TrainingOptions
from .configuration import (
    DEFAULT_TRAINING_MAP as DEFAULT_TRAINING_MAP,
)
from .configuration import (
    WORKER_LOCKSTEP_PHYSICS_ARGS as WORKER_LOCKSTEP_PHYSICS_ARGS,
)
from .configuration import (
    build_launch_overrides as build_launch_overrides,
)
from .configuration import (
    build_run_config as build_run_config,
)
from .rsl_rl import UERLOnPolicyRunner, UERLVecEnvWrapper
from .rsl_rl.time_aware_ppo import PHANTOMX_PHYSICAL_TIME_OBJECTIVE

_CONFIG_PATH = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)+$")
# Adaptive Runs register the terrain term under this name; fixed-level Runs omit it.
_TERRAIN_CURRICULUM_TERM = "terrain"
# Keep UE/Session state and Task mathematics on CPU. RSL-RL receives this
# environment device separately and moves observations/actions to its runner
# device for model inference and PPO updates.
ENVIRONMENT_DEVICE = "cpu"


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
    task.capabilities.require("train", allow_unknown=True)
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
    if resume_checkpoint is not None and not resume_checkpoint.is_file():
        raise FileNotFoundError(resume_checkpoint)
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
        capabilities = task.capabilities
        capabilities.require("train")
        print(f"[CAPABILITY] {capabilities.format()}")
        train_config = build_rsl_rl_train_config(config)
        vec_env = UERLVecEnvWrapper(direct_env, cfg=config)
        runner = UERLOnPolicyRunner(
            vec_env,
            train_config,
            log_dir=str(metrics_directory),
            device=config.runner.device,
        )
        runner.resolved_config = config
        runner.training_options = TrainingOptions(terrain_level, freeze_observation_normalization).to_dict()
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
    trace_path: Path | None = None,
    terrain_level: int | None = None,
    evaluator_factory: Callable[[float], TaskEvaluator | None] | None = None,
    restore_curriculum: bool = True,
    play_controller: str = "task",
    fixed_velocity: tuple[float, float, float] | None = None,
) -> object:
    """Run a checkpoint deterministically and aggregate fixed-horizon metrics.

    ``terrain_level`` fixes the procedural terrain tier for playback; leaving
    it unset preserves the task's adaptive curriculum.
    """

    import torch
    if steps < 1:
        raise ValueError("steps must be positive")
    if fixed_velocity is not None and play_controller != "fixed":
        raise ValueError("fixed_velocity requires play_controller='fixed'")
    if not checkpoint.is_file():
        raise FileNotFoundError(checkpoint)
    torch.manual_seed(config.worker.run_seed)

    trace_recorder = None
    train_config = build_rsl_rl_train_config(config)
    if trace_path is not None:
        if config.worker.slot_count != 1:
            raise ValueError("policy step tracing currently supports exactly one Slot")
        from .policy_trace import TaskPolicyTraceRecorder

        actor_observation_groups = tuple(train_config["obs_groups"]["actor"])
        trace_recorder = TaskPolicyTraceRecorder(
            trace_path,
            source={
                "task_id": config.task_id,
                "robot_id": config.worker.robot_id,
                "run_directory": config.logging.run_directory,
                "checkpoint": checkpoint,
                "map_package": config.session.map_path,
                "seed": config.worker.run_seed,
                "controller": play_controller,
                "observation_normalization": bool(
                    config.runner.parameters.get("obs_normalization", False)
                ),
            },
            clock={
                "physics_dt_s": config.worker.physics_dt,
                "decimation_range": list(config.worker.decimation),
            },
            actor_observation_groups=actor_observation_groups,
        )

    raw_session: UERLSession | None = None
    direct_env: UERLDirectEnv | None = None
    vec_env: UERLVecEnvWrapper | None = None
    evaluation_complete = False
    try:
        registry = create_default_registry()
        task = registry.create_task(config.task_id, config.task)
        task.capabilities.require("evaluate", allow_unknown=True)
        if trace_recorder is not None:
            # Paired traces fingerprint the exported policy and its deployment plan.
            task.capabilities.require("export", allow_unknown=True)
        task.align_batch(config.worker.slot_count)
        player_controller = None
        if play_controller != "task":
            from ..tasks.controllers import PlayerVelocityController, create_play_controller

            replacement = create_play_controller(
                play_controller,
                task,
                config.worker.slot_count,
                fixed_velocity=fixed_velocity,
            )
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
        direct_env = UERLDirectEnv(
            UERLSessionAdapter(raw_session, device=ENVIRONMENT_DEVICE),
            task,
            resolved_config=config,
            device=ENVIRONMENT_DEVICE,
            curriculum_manager=curriculum_manager,
            initial_terrain_level=terrain_level,
            step_trace_callback=None if trace_recorder is None else trace_recorder.record_step,
        )
        capabilities = task.capabilities
        capabilities.require("evaluate")
        print(f"[CAPABILITY] {capabilities.format()}")
        if trace_recorder is not None:
            trace_recorder.set_deployment_state_fields(
                tuple(task.observation_plan.state_requirements)
            )
        vec_env = UERLVecEnvWrapper(direct_env, cfg=config)
        runner = UERLOnPolicyRunner(
            vec_env,
            train_config,
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
        if trace_recorder is not None:
            trace_recorder.set_policy_fingerprint(
                _export_policy_onnx_sha1(task=task, runner=runner, config=config)
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
        evaluation_complete = True
    finally:
        try:
            if vec_env is not None:
                vec_env.close("evaluation_complete")
            elif direct_env is not None:
                direct_env.close("evaluation_setup_failed")
            elif raw_session is not None:
                raw_session.close("evaluation_setup_failed")
        except BaseException:
            evaluation_complete = False
            raise
        finally:
            if trace_recorder is not None:
                trace_recorder.finish(complete=evaluation_complete)

    summary = EvaluationSummary(
        task_id=config.task_id,
        checkpoint=checkpoint,
        steps=steps,
        completed_episodes=completed_episodes,
        mean_episode_length=episode_length_sum / completed_episodes if completed_episodes else 0.0,
        mean_reward=reward_sum / steps,
    )
    return evaluator.finish(summary) if evaluator is not None else summary


def _export_policy_onnx_sha1(
    *,
    task: DirectTask,
    runner: UERLOnPolicyRunner,
    config: ResolvedRunConfig,
) -> str:
    """Fingerprint the exact ONNX payload that the UE artifact must contain."""

    import hashlib
    import tempfile

    from ..core.config.manifest import capture_git_identity
    from ..policy.artifact import ArtifactMetadata, ArtifactTiming
    from .export import export_policy, robot_runtime_from_config

    with tempfile.TemporaryDirectory(prefix="uerl-trace-artifact-") as temporary_directory:
        artifact = export_policy(
            task=task,
            runner=runner,
            output=Path(temporary_directory) / "trace-policy.uerlpol2",
            metadata=ArtifactMetadata(
                run_hash=config.normalized_hash,
                git_identity=dict(capture_git_identity()),
            ),
            robot_runtime=robot_runtime_from_config(config),
            timing=ArtifactTiming(
                config.worker.physics_dt,
                config.worker.decimation[0],
                config.worker.decimation[1],
            ),
            task_id=config.task_id,
            robot_id=config.worker.robot_id,
        )
    return hashlib.sha1(artifact.onnx).hexdigest()


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

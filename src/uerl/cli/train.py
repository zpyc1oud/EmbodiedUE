"""Train one registered Task through the generic UE-RL PPO workflow."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .boundary import guard, parse_overrides
from .flags import TRAIN_FLAGS, add_common_flags, apply_common_flags
from .host_flags import add_host_flags, resolve_host_flags


def _parser() -> argparse.ArgumentParser:
    """Build the fixed portion of the generic training parser."""

    parser = argparse.ArgumentParser(
        description="Train one registered UE-RL Task with RSL-RL PPO.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Everyday flags: --num-envs, --seed, --device, --max-iterations. "
            "Other config uses repeated --<dotted-path> <value> entries."
        ),
    )
    parser.add_argument("--task", required=True, help="Registered Task ID.")
    parser.add_argument("--debug-trace", type=Path,
                        help="Stream every Slot's control-step and PPO evidence to a new YAML file.")
    parser.add_argument("--debug-rollouts", type=int, default=2,
                        help="Capture this many initial full rollouts (default: 2). Does not stop training.")
    add_common_flags(parser, TRAIN_FLAGS)
    parser.add_argument(
        "--run-dir",
        type=Path,
        help="Output directory for this Run; defaults to a new task-scoped timestamped directory.",
    )
    parser.add_argument(
        "--resume",
        help=(
            "Continue from a checkpoint with its complete Run configuration, a Run directory, or 'latest'. "
            "A Run uses model_final.pt, or the highest rsl_rl/model_<iteration>.pt "
            "when training stopped before the final file was written."
        ),
    )
    parser.add_argument(
        "--resume-normalization", choices=("update", "frozen"),
        help="Recover the normalization update policy of an old Run with no recorded policy.",
    )
    parser.add_argument(
        "--run-name",
        help="Readable name used in the default Run directory.",
    )
    add_host_flags(parser)
    parser.add_argument(
        "--map",
        dest="map_name",
        help="UE World long package name; continuation preserves the recorded map.",
    )
    parser.add_argument(
        "--presentation",
        choices=("none", "viewport", "gameplay"),
        default="none",
        help="Worker presentation: 'none' launches headless; 'viewport' opens a UE5 window.",
    )
    parser.add_argument("--res-x", type=int, default=640, help="Viewport width in pixels (viewport only).")
    parser.add_argument("--res-y", type=int, default=360, help="Viewport height in pixels (viewport only).")
    parser.add_argument(
        "--terrain-level",
        type=int,
        help="Fix every training Slot to this zero-based terrain level and disable terrain curriculum.",
    )
    return parser


@guard
def main(argv: list[str] | None = None) -> int:
    """Resolve one Run, execute generic PPO, and print durable evidence."""

    from ..application.continuation import Continuation
    from ..core.config import PresentationMode
    from ..core.config.snapshot import decode_worker_args, encode_worker_args
    from ..training import (
        build_launch_overrides,
        build_run_config,
        make_run_directory,
        record_command,
        resolve_resume_checkpoint,
        run_training,
    )
    from ..training.checkpoint import inspect_resume_checkpoint

    parser = _parser()
    args, remaining = parser.parse_known_args(argv)
    if args.debug_rollouts < 1:
        parser.error("--debug-rollouts must be positive")
    direct_overrides = apply_common_flags(
        parser,
        args,
        parse_overrides(parser, remaining),
        TRAIN_FLAGS,
    )
    if args.run_dir is not None and "logging.run_directory" in direct_overrides:
        parser.error("--run-dir cannot be combined with --logging.run_directory")
    if args.resume is not None and "runner.checkpoint" in direct_overrides:
        parser.error("--resume cannot be combined with --runner.checkpoint")
    reference = args.resume or direct_overrides.get("runner.checkpoint")
    continuation = None
    if reference is not None:
        checkpoint = resolve_resume_checkpoint(reference, task_id=args.task)
        continuation = Continuation.open(args.task, checkpoint)
    elif args.resume_normalization is not None:
        parser.error("--resume-normalization requires --resume or --runner.checkpoint")
    if args.map_name:
        direct_overrides["session.map_path"] = args.map_name
    base_config = (
        continuation.resolve(direct_overrides) if continuation is not None
        else build_run_config(args.task, overrides=direct_overrides)
    )
    resume_state = None if continuation is None else inspect_resume_checkpoint(
        base_config, terrain_level=args.terrain_level, normalization=args.resume_normalization,
    )
    map_name = base_config.session.map_path
    launch_overrides: dict[str, str] = {}
    if direct_overrides.get("session.mode") != "attach":
        host = resolve_host_flags(args)
        launch_overrides = build_launch_overrides(
            ue_executable=host.ue_executable,
            project=host.project,
            map_name=map_name,
            presentation=PresentationMode(args.presentation),
            window_size=(args.res_x, args.res_y),
        )
        explicit_port = direct_overrides.get("session.port")
        if explicit_port is not None:
            worker_args = decode_worker_args(launch_overrides["session.worker_args"])
            launch_overrides["session.worker_args"] = encode_worker_args(
                [f"-uerlport={explicit_port}" if item.startswith("-uerlport=") else item for item in worker_args]
            )
    generated_output = False
    if args.run_dir is not None:
        direct_overrides["logging.run_directory"] = str(args.run_dir)
    elif "logging.run_directory" not in direct_overrides:
        generated_output = True
        configured_name = base_config.runner.parameters.get("run_name", "run")
        run_directory = make_run_directory(
            args.task,
            run_name=args.run_name or str(configured_name),
        )
        direct_overrides["logging.run_directory"] = str(run_directory)
    overrides = {**launch_overrides, **direct_overrides, "session.map_path": map_name}
    config = (
        continuation.resolve(direct_overrides, launch_overrides=launch_overrides)
        if continuation is not None else build_run_config(args.task, overrides=overrides)
    )
    if continuation is not None:
        continuation.validate_output(config.logging.run_directory)
        print(f"[RUN] config={continuation.source.source_path} intent=continuation")
        report_overrides = dict(direct_overrides)
        if generated_output:
            del report_overrides["logging.run_directory"]
        for change in continuation.changes(config, report_overrides, launch_overrides):
            print(f"[CONFIG] {change.path}: {json.dumps(change.saved)} -> {json.dumps(change.effective)} "
                  f"source={change.source}")
    if resume_state is not None:
        random_text = "restore" if resume_state.restores_decimation_rng else "not_recorded_fixed_interval"
        print(f"[RESTORE] actor=restore critic=restore optimizer=restore iteration={resume_state.iteration} "
              f"normalization={'frozen' if resume_state.options.freeze_observation_normalization else 'update'} "
              f"curriculum={','.join(resume_state.curriculum_terms) or 'none'} "
              f"decimation_rng={random_text} "
              f"terrain_level={resume_state.options.terrain_level} options_source={resume_state.options_source} "
              "episodes=fresh full_rng=not_saved physics_state=not_saved")
    record_command(config.logging.run_directory, list(sys.argv[1:] if argv is None else argv))
    resume_text = f" resume={config.runner.checkpoint}" if config.runner.checkpoint is not None else " resume=none"
    print(f"[RUN] directory={config.logging.run_directory}{resume_text}")
    debug_options = (
        {} if args.debug_trace is None
        else {"debug_trace_path": args.debug_trace, "debug_rollouts": args.debug_rollouts}
    )
    if resume_state is None:
        result = run_training(config, terrain_level=args.terrain_level, **debug_options)
    else:
        result = run_training(
            config, terrain_level=resume_state.options.terrain_level,
            freeze_observation_normalization=resume_state.options.freeze_observation_normalization,
            **debug_options,
        )
    print(
        f"[VERIFY] VC-007: task={result.task_id} iterations={result.iterations} "
        f"checkpoint={result.checkpoint} metrics={result.metrics_directory} "
        f"config_hash={result.normalized_hash} shutdown=PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

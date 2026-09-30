"""Train one registered Task through the generic UE-RL PPO workflow."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .boundary import guard, parse_overrides
from .flags import TRAIN_FLAGS, add_common_flags, apply_common_flags

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_UE_EXECUTABLE = Path(
    r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
)


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
    add_common_flags(parser, TRAIN_FLAGS)
    parser.add_argument(
        "--run-dir",
        type=Path,
        help="Output directory for this Run; defaults to a new task-scoped timestamped directory.",
    )
    parser.add_argument(
        "--resume",
        help=(
            "Checkpoint file, Run directory, or 'latest'. "
            "A Run uses model_final.pt, or the highest rsl_rl/model_<iteration>.pt "
            "when training stopped before the final file was written."
        ),
    )
    parser.add_argument(
        "--run-name",
        help="Readable name used in the default Run directory.",
    )
    parser.add_argument(
        "--ue-executable",
        type=Path,
        default=DEFAULT_UE_EXECUTABLE,
        help="Path to UnrealEditor-Cmd.exe.",
    )
    parser.add_argument(
        "--project",
        type=Path,
        default=REPO_ROOT / "engine" / "UERLHost.uproject",
        help="UE project containing the UERL plugin.",
    )
    parser.add_argument(
        "--map",
        dest="map_name",
        help="UE World long package name; defaults to the registered Task map.",
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

    from ..core.config import PresentationMode
    from ..training import (
        build_launch_overrides,
        build_run_config,
        make_run_directory,
        record_command,
        resolve_resume_checkpoint,
        run_training,
    )

    parser = _parser()
    args, remaining = parser.parse_known_args(argv)
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
    if args.resume is not None:
        direct_overrides["runner.checkpoint"] = str(
            resolve_resume_checkpoint(args.resume, task_id=args.task)
        )

    base_config = build_run_config(args.task, overrides=direct_overrides)
    map_name = (
        args.map_name
        or direct_overrides.get("session.map_path")
        or base_config.session.map_path
    )
    launch_overrides: dict[str, str] = {}
    if direct_overrides.get("session.mode") != "attach":
        launch_overrides = build_launch_overrides(
            ue_executable=args.ue_executable,
            project=args.project,
            map_name=map_name,
            presentation=PresentationMode(args.presentation),
            window_size=(args.res_x, args.res_y),
        )
        explicit_port = direct_overrides.get("session.port")
        if explicit_port is not None:
            worker_args = json.loads(launch_overrides["session.worker_args"])
            launch_overrides["session.worker_args"] = json.dumps(
                [
                    f"-uerlport={explicit_port}" if item.startswith("-uerlport=") else item
                    for item in worker_args
                ]
            )
    if args.run_dir is not None:
        direct_overrides["logging.run_directory"] = str(args.run_dir)
    elif "logging.run_directory" not in direct_overrides:
        configured_name = base_config.runner.parameters.get("run_name", "run")
        run_directory = make_run_directory(
            args.task,
            run_name=args.run_name or str(configured_name),
        )
        direct_overrides["logging.run_directory"] = str(run_directory)
    overrides = {**launch_overrides, **direct_overrides, "session.map_path": map_name}
    config = build_run_config(args.task, overrides=overrides)
    record_command(config.logging.run_directory, list(sys.argv[1:] if argv is None else argv))
    resume_text = f" resume={config.runner.checkpoint}" if config.runner.checkpoint is not None else " resume=none"
    print(f"[RUN] directory={config.logging.run_directory}{resume_text}")
    result = run_training(config, terrain_level=args.terrain_level)
    print(
        f"[VERIFY] VC-007: task={result.task_id} iterations={result.iterations} "
        f"checkpoint={result.checkpoint} metrics={result.metrics_directory} "
        f"config_hash={result.normalized_hash} shutdown=PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

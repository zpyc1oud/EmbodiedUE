"""Play and evaluate one registered UE-RL checkpoint."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import cast

from ..application.run_config import (
    find_run_directory_for_checkpoint,
    load_run_config_source,
)
from ..core.config.canonical import canonical_json
from ..core.config.manifest import capture_git_identity
from ..core.config.snapshot import decode_worker_args, encode_worker_args
from ..host.profile import DEFAULT_UE_EXECUTABLE
from ..presentation import InternalViewportRecorder
from .boundary import guard, parse_overrides, validate_saved_run_overrides
from .flags import PLAY_FLAGS, add_common_flags, apply_common_flags
from .host_flags import add_host_flags, resolve_host_flags

__all__ = ["DEFAULT_UE_EXECUTABLE", "InternalViewportRecorder", "_parser", "main"]


def _write_record_evidence(
    recorder: InternalViewportRecorder,
    result: object,
    *,
    terrain_level: int | None,
) -> Path:
    """Persist one evaluation and its UE-internal recording metadata beside the video."""

    evidence_path = recorder.output.with_suffix(".json")
    payload = {
        "evaluation": result,
        "git_identity": capture_git_identity(),
        "record": {
            "capture": "ue-internal",
            "fps": recorder.fps,
            "frames": recorder.frame_count,
            "recording": "PASS",
            "resolution": recorder.size,
            "terrain_level": terrain_level,
            "termination_events": getattr(result, "termination_events", ()),
            "video": recorder.output,
        },
    }
    evidence_path.write_text(canonical_json(payload) + "\n", encoding="utf-8")
    return evidence_path


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run a trained UE-RL policy deterministically.")
    parser.add_argument(
        "--task",
        help="Optional with a Run snapshot; required for 'latest' and must match the saved Task ID.",
    )
    add_common_flags(parser, PLAY_FLAGS)
    parser.add_argument(
        "--checkpoint",
        type=Path,
        help="RSL-RL checkpoint; its saved Run config may be embedded or found in an ancestor Run.",
    )
    parser.add_argument(
        "--run",
        help=(
            "Run directory or 'latest'. Uses model_final.pt, else the highest rsl_rl/model_<iteration>.pt, "
            "and saved Run config for Task identity and trained settings. 'latest' requires --task."
        ),
    )
    parser.add_argument("--steps", type=int, default=500, help="Fixed evaluation horizon.")
    parser.add_argument(
        "--terrain-level",
        type=int,
        help="Fix the procedural terrain level for playback; omit to use the task curriculum.",
    )
    add_host_flags(parser)
    parser.add_argument(
        "--map",
        dest="map_name",
        help="UE World long package name; defaults to the recorded Run map, then the registered Task map.",
    )
    parser.add_argument(
        "--presentation",
        choices=("none", "viewport", "gameplay"),
        default="viewport",
        help="Use gameplay to preserve the map's playable Pawn; viewport installs the UERL observer.",
    )
    parser.add_argument("--res-x", type=int, default=1280)
    parser.add_argument("--res-y", type=int, default=720)
    parser.add_argument(
        "--record",
        type=Path,
        help="Encode frames captured from the UE renderer into this MP4 path.",
    )
    parser.add_argument(
        "--trace",
        type=Path,
        help="Write paired Task/UE step evidence as YAML; requires Task export plans.",
    )
    parser.add_argument(
        "--controller",
        default="task",
        help=(
            "Play command publisher. 'task' keeps task sampling; 'player' reads keys; "
            "'fixed' publishes the explicit --fixed-velocity on every step and reset."
        ),
    )
    parser.add_argument(
        "--fixed-velocity",
        help="Three comma-separated values for the fixed velocity controller, for example 0.45,0,0.",
    )
    parser.add_argument("--record-seconds", type=float, default=20.0, help="Maximum recorded duration.")
    parser.add_argument("--record-fps", type=int, default=30, help="Encoded video frame rate.")
    return parser


@guard
def main(argv: list[str] | None = None) -> int:
    from ..core.config import PresentationMode
    from ..tasks.evaluation import EvaluationSummary, format_generic_evaluation
    from ..tasks.registry import create_default_registry
    from ..training import (
        build_launch_overrides,
        resolve_resume_checkpoint,
        resolve_run_directory,
        run_evaluation,
    )

    parser = _parser()
    args, remaining = parser.parse_known_args(argv)
    if args.run is not None and args.checkpoint is not None:
        parser.error("--run cannot be combined with --checkpoint")
    if args.run is None and args.checkpoint is None:
        parser.error("one of --run or --checkpoint is required")
    if args.run == "latest" and args.task is None:
        parser.error("--task is required with --run latest because latest is selected within a Task")
    if args.run is None:
        checkpoint = args.checkpoint
        assert checkpoint is not None
        run_directory = find_run_directory_for_checkpoint(checkpoint)
    else:
        run_directory = resolve_run_directory(args.run, task_id=args.task)
        checkpoint = resolve_resume_checkpoint(run_directory, task_id=args.task)
    if not checkpoint.is_file():
        parser.error(f"checkpoint not found: {checkpoint}")
    print(f"[RUN] checkpoint={checkpoint}")
    direct_overrides = apply_common_flags(parser, args, parse_overrides(parser, remaining), PLAY_FLAGS)
    direct_overrides.setdefault("worker.slot_count", "1")
    from ..tasks.controllers import known_play_controllers

    if args.controller not in known_play_controllers():
        parser.error(f"unknown play controller {args.controller!r}")
    fixed_velocity = None
    if args.controller == "fixed":
        if args.fixed_velocity is None:
            parser.error("--controller fixed requires --fixed-velocity FORWARD,LATERAL,YAW_RATE")
        try:
            fixed_values = tuple(float(value.strip()) for value in args.fixed_velocity.split(","))
        except ValueError:
            parser.error("--fixed-velocity must contain three comma-separated numbers")
        if len(fixed_values) != 3:
            parser.error("--fixed-velocity must contain exactly three comma-separated numbers")
        fixed_velocity = (fixed_values[0], fixed_values[1], fixed_values[2])
    elif args.fixed_velocity is not None:
        parser.error("--fixed-velocity requires --controller fixed")
    if args.map_name:
        direct_overrides["session.map_path"] = args.map_name
    validate_saved_run_overrides(parser, direct_overrides, operation="play")
    run_config_source = load_run_config_source(args.task, run_directory, strict=True, checkpoint=checkpoint)
    base_config = run_config_source.resolve(direct_overrides).config
    if base_config.worker.slot_count != 1:
        parser.error("play controls one robot")
    map_name = base_config.session.map_path
    recorder = None
    if args.record is not None:
        if args.presentation != "viewport":
            parser.error("--record requires --presentation viewport")
        if direct_overrides.get("session.mode") == "attach":
            parser.error("--record requires play to launch a new viewport")
        if args.record_seconds <= 0.0:
            parser.error("--record-seconds must be positive")
        if args.record_fps < 1:
            parser.error("--record-fps must be positive")
        if args.res_x < 1 or args.res_y < 1:
            parser.error("--res-x and --res-y must be positive")
        recorder = InternalViewportRecorder(
            args.record,
            fps=args.record_fps,
            seconds=args.record_seconds,
            size=(args.res_x, args.res_y),
        )
        recorder.start()

    try:
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
            if explicit_port is not None or recorder is not None:
                worker_args = decode_worker_args(launch_overrides["session.worker_args"])
                if explicit_port is not None:
                    worker_args = [
                        f"-uerlport={explicit_port}" if item.startswith("-uerlport=") else item
                        for item in worker_args
                    ]
                if recorder is not None:
                    worker_args.extend(recorder.worker_arguments())
                launch_overrides["session.worker_args"] = encode_worker_args(worker_args)
        resolved = run_config_source.resolve(
            {**launch_overrides, **direct_overrides, "session.map_path": map_name}
        )
        config = resolved.config
        config_source = resolved.source_path or "task defaults"
        print(f"[RUN] config={config_source}")
    except BaseException:
        if recorder is not None:
            recorder.abort()
        raise

    try:
        result = run_evaluation(
            config,
            checkpoint=checkpoint,
            steps=args.steps,
            terrain_level=args.terrain_level,
            # Playback is one Slot; checkpoint curriculum state is per-Slot training state.
            restore_curriculum=False,
            play_controller=args.controller,
            fixed_velocity=fixed_velocity,
            trace_path=args.trace,
        )
    except BaseException:
        if recorder is not None:
            recorder.stop(raise_on_error=False)
        raise
    if recorder is not None:
        recorder.stop()
        record_evidence = _write_record_evidence(
            recorder,
            result,
            terrain_level=args.terrain_level,
        )
    else:
        record_evidence = None
    task_id = cast(EvaluationSummary, result).task_id
    formatted = create_default_registry().format_evaluation(task_id, result)
    line = formatted or format_generic_evaluation(cast(EvaluationSummary, result))
    terrain_text = f"terrain_level={args.terrain_level if args.terrain_level is not None else 'auto'}"
    print(line.replace("shutdown=PASS", f"{terrain_text} shutdown=PASS"))
    if recorder is not None:
        print(
            f"[VERIFY] video={recorder.output} bytes={recorder.output.stat().st_size} "
            f"frames={recorder.frame_count} capture=ue-internal recording=PASS"
        )
        print(f"[VERIFY] record_metadata={record_evidence} persistence=PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

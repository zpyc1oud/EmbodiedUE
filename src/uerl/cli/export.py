"""Export a trained RSL-RL checkpoint into a UERLPOL2 policy artifact."""

from __future__ import annotations

import argparse
from pathlib import Path

import yaml

from uerl.application.run_config import (
    find_run_directory_for_checkpoint,
    load_run_config_source,
)
from uerl.cli.boundary import guard, parse_overrides, validate_saved_run_overrides
from uerl.cli.flags import EXPORT_FLAGS, add_common_flags, apply_common_flags
from uerl.cli.host_flags import add_host_flags, resolve_host_flags
from uerl.core.config.snapshot import decode_worker_args, encode_worker_args
from uerl.core.config.yaml_loader import load_unique_yaml
from uerl.training.export import export_policy


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Export one RSL-RL checkpoint to a UERLPOL2 artifact "
            "(plans + ONNX from runner.export_policy_to_onnx)."
        ),
    )
    parser.add_argument(
        "--task",
        help="Optional with a Run snapshot; required for 'latest' and must match the saved Task ID.",
    )
    add_common_flags(parser, EXPORT_FLAGS)
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
    parser.add_argument("--output", type=Path, help="Destination .uerlpol2 path.")
    parser.add_argument(
        "--robot-runtime",
        type=Path,
        help=(
            "Optional YAML or legacy JSON file with the artifact robot_runtime segment. "
            "Defaults to the resolved Task RobotConfig."
        ),
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
        default="none",
        help="Worker presentation; export defaults to headless.",
    )
    parser.add_argument("--res-x", type=int, default=640)
    parser.add_argument("--res-y", type=int, default=360)
    return parser


@guard
def main(argv: list[str] | None = None) -> int:
    """Load a checkpoint, export ONNX via RSL-RL, and write a UERLPOL2 artifact."""

    from uerl.core.config import PresentationMode
    from uerl.core.config.manifest import capture_git_identity
    from uerl.core.direct.env import UERLDirectEnv
    from uerl.policy.artifact import ArtifactMetadata, ArtifactTiming, RobotRuntime
    from uerl.runtime.session import UERLSession, UERLSessionAdapter, WorkerProcessController
    from uerl.tasks.registry import create_default_registry
    from uerl.training import (
        build_launch_overrides,
        build_rsl_rl_train_config,
        resolve_resume_checkpoint,
        resolve_run_directory,
        robot_runtime_from_config,
    )
    from uerl.training.rsl_rl import UERLOnPolicyRunner, UERLVecEnvWrapper
    from uerl.training.runner import ENVIRONMENT_DEVICE

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

    direct_overrides = apply_common_flags(parser, args, parse_overrides(parser, remaining), EXPORT_FLAGS)
    # The artifact does not depend on the Slot count; one Robot is enough to build the policy graph.
    direct_overrides.setdefault("worker.slot_count", "1")
    if args.map_name:
        direct_overrides["session.map_path"] = args.map_name
    validate_saved_run_overrides(parser, direct_overrides, operation="export")
    run_config_source = load_run_config_source(args.task, run_directory, strict=True, checkpoint=checkpoint)
    base_config = run_config_source.resolve(direct_overrides).config
    if base_config.worker.slot_count != 1:
        parser.error("export uses one robot to build the artifact")
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
    resolved = run_config_source.resolve(
        {**launch_overrides, **direct_overrides, "session.map_path": map_name}
    )
    config = resolved.config
    config_source = resolved.source_path or "task defaults"
    print(f"[RUN] config={config_source} run_hash={resolved.run_hash}")
    if args.output is None:
        if run_directory is None:
            parser.error("--output is required when --checkpoint is detached from a saved Run")
        output = run_directory / "exported" / f"{config.task_id}.uerlpol2"
    else:
        output = args.output
    if args.robot_runtime is None:
        robot_runtime = robot_runtime_from_config(config)
    else:
        if not args.robot_runtime.is_file():
            parser.error("--robot-runtime must point to an existing YAML file")
        try:
            runtime_payload = load_unique_yaml(args.robot_runtime.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, yaml.YAMLError, TypeError, ValueError) as exc:
            parser.error(f"cannot read --robot-runtime: {exc}")
        if not isinstance(runtime_payload, dict):
            parser.error("--robot-runtime must contain a YAML mapping")
        if any(not isinstance(key, str) for key in runtime_payload):
            parser.error("--robot-runtime mapping keys must be strings")
        robot_runtime = RobotRuntime.from_json(runtime_payload)

    registry = create_default_registry()
    registration = registry.resolve(config.task_id)
    task = registry.create_task(config.task_id, config.task)
    metadata = ArtifactMetadata(
        run_hash=resolved.run_hash,
        git_identity=dict(capture_git_identity()),
    )

    curriculum_manager = registry.create_curriculum(
        config.task_id,
        config.task,
        num_envs=config.worker.slot_count,
        device=ENVIRONMENT_DEVICE,
        run_seed=config.worker.run_seed,
    )
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
        )
        vec_env = UERLVecEnvWrapper(direct_env, cfg=config)
        runner = UERLOnPolicyRunner(
            vec_env,
            build_rsl_rl_train_config(config),
            log_dir=None,
            device=config.runner.device,
        )
        # Export only needs the policy graph.  Curriculum state is unrelated to
        # ONNX weights and a terrain checkpoint may contain a term that this
        # export environment intentionally does not instantiate.
        runner.load(
            str(checkpoint),
            map_location=config.runner.device,
            restore_curriculum=False,
        )
        artifact = export_policy(
            task=task,
            runner=runner,
            output=output,
            metadata=metadata,
            robot_runtime=robot_runtime,
            timing=ArtifactTiming(
                config.worker.physics_dt,
                config.worker.decimation[0],
                config.worker.decimation[1],
            ),
            task_id=config.task_id,
            robot_id=registration.robot_id,
        )
    finally:
        if vec_env is not None:
            vec_env.close("export_complete")
        elif direct_env is not None:
            direct_env.close("export_setup_failed")
        else:
            raw_session.close("export_setup_failed")

    print(
        f"[VERIFY] export: task={artifact.task_id} robot={artifact.robot_id} "
        f"output={output} onnx_nbytes={len(artifact.onnx)} "
        f"path=onnx shutdown=PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

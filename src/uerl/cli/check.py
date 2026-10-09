"""Run pure-Python preflight checks for registered Tasks."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import yaml

from ..application.run_config import resolve_run_config
from ..core.direct.capabilities import TaskCapabilities
from ..errors import ConfigError, RegistryError, UERLError
from ..policy.artifact import ArtifactTiming, PolicyArtifact
from ..tasks.registry import create_default_registry
from ..training import robot_runtime_from_config
from .boundary import describe, parse_overrides
from .host_check import add_host_arguments, run_host_check


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run UE-RL preflight checks without starting Unreal Engine.")
    subparsers = parser.add_subparsers(dest="kind", required=True)
    task_parser = subparsers.add_parser("task", help="check one registered Task")
    task_parser.add_argument("task_id", help="Registered Task ID.")
    task_parser.add_argument(
        "--json",
        action="store_true",
        help="Print the preflight report as JSON; it is not a Task configuration file.",
    )
    task_parser.add_argument("--yaml", action="store_true", help="Print the preflight report as YAML.")
    task_parser.add_argument("--run", type=Path, help="Check the saved Run configuration instead of defaults.")
    task_parser.add_argument(
        "--artifact", type=Path, help="Compare artifact timing and actuators; validate ONNX widths."
    )
    host_parser = subparsers.add_parser("host", help="check the bundled CartPole host files and profile")
    add_host_arguments(host_parser)
    return parser


def _task_report(
    task_id: str,
    *,
    overrides: dict[str, str] | None = None,
    run: Path | None = None,
    artifact_path: Path | None = None,
) -> tuple[dict[str, object], TaskCapabilities]:
    registry = create_default_registry()
    registration = registry.resolve(task_id)
    if run is not None and overrides:
        raise ConfigError("saved Run checks do not accept overrides", path="run", code="PREFLIGHT_OVERRIDE")
    config = resolve_run_config(task_id, run, overrides or {}, strict=run is not None).config
    task = registry.create_task(task_id, config.task)
    capabilities = task.capabilities
    runtime = robot_runtime_from_config(config)
    artifact_report: dict[str, object] | None = None
    if artifact_path is not None:
        artifact = PolicyArtifact.read(artifact_path)
        artifact.validate()
        expected_timing = ArtifactTiming(config.worker.physics_dt, *config.worker.decimation)
        for field, expected, actual in (
            ("task_id", config.task_id, artifact.task_id),
            ("robot_id", config.worker.robot_id, artifact.robot_id),
            ("timing", expected_timing, artifact.timing),
            ("robot_runtime", runtime, artifact.robot_runtime),
        ):
            if expected != actual:
                raise ConfigError(
                    f"artifact {field} differs from the resolved configuration",
                    path=f"artifact.{field}",
                    code="PREFLIGHT_ARTIFACT_MISMATCH",
                )
        artifact_report = {
            "path": str(artifact_path),
            "policy_observation_width": artifact.observation_plan.group_widths["policy"],
            "policy_action_width": artifact.action_plan.policy_width,
            "checked": ["task_id", "robot_id", "timing", "robot_runtime", "ONNX/plan widths"],
            "pending": ["bound Task plan equality", "native mesh topology", "scene input binding"],
        }
    return {
        "task_id": config.task_id,
        "task_version": config.task_version,
        "environment_id": registration.environment_id,
        "robot_id": registration.robot_id,
        "map_path": config.session.map_path,
        "slot_count": config.worker.slot_count,
        "device": config.runner.device,
        "max_iterations": config.runner.max_iterations,
        "physics_dt_s": config.worker.physics_dt,
        "decimation": list(config.worker.decimation),
        "control_interval_s": [config.worker.physics_dt * value for value in config.worker.decimation],
        "actuator_joints": [actuator.joint for actuator in runtime.actuators],
        "artifact": artifact_report,
        "pending_host_checks": ["native joint/body binding", "bound observation dimensions", "scene collision"],
        "actuator_count": len(runtime.actuators),
        "state_requirements": list(config.task.state_requirements),
        "action_schema": list(config.task.action_schema),
        "config_hash": config.normalized_hash,
        "capabilities": capabilities.to_dict(),
    }, capabilities


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args, remaining = parser.parse_known_args(argv)
    if args.kind == "host":
        if remaining:
            parser.error(f"unrecognized arguments: {' '.join(remaining)}")
        return run_host_check(args)
    if args.kind != "task":
        return 2
    if args.json and args.yaml:
        parser.error("choose --yaml or --json, not both")
    overrides = parse_overrides(parser, remaining)
    try:
        report, capabilities = _task_report(
            args.task_id, overrides=overrides, run=args.run, artifact_path=args.artifact
        )
    except RegistryError as exc:
        for line in describe(exc):
            print(line)
        return 1
    except ConfigError as exc:
        for line in describe(exc):
            print(line)
        return 1
    except (UERLError, OSError) as exc:
        print(f"[FAIL] task preflight: {exc}")
        return 1
    if args.yaml:
        print(yaml.safe_dump(report, sort_keys=False), end="")
    elif args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print(
            f"[PASS] task={report['task_id']} version={report['task_version']} "
            f"robot={report['robot_id']} environment={report['environment_id']}"
        )
        print(
            f"map={report['map_path']} slots={report['slot_count']} device={report['device']} "
            f"max_iterations={report['max_iterations']} actuators={report['actuator_count']} "
            f"config_hash={report['config_hash']}"
        )
        print(f"physics_dt_s={report['physics_dt_s']} control_interval_s={report['control_interval_s']}")
        print(f"pending_host_checks={report['pending_host_checks']}")
        if report["artifact"] is not None:
            print(f"artifact={report['artifact']}")
        print(f"capabilities {capabilities.format()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

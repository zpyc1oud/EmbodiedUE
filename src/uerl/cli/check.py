"""Run pure-Python preflight checks for registered Tasks."""

from __future__ import annotations

import argparse
import json

from ..errors import ConfigError, RegistryError, UERLError
from ..tasks.registry import create_default_registry
from ..training import build_run_config, robot_runtime_from_config
from .boundary import suggest_task_ids


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run UE-RL preflight checks without starting Unreal Engine.")
    subparsers = parser.add_subparsers(dest="kind", required=True)
    task_parser = subparsers.add_parser("task", help="check one registered Task")
    task_parser.add_argument("task_id", help="Registered Task ID.")
    task_parser.add_argument("--json", action="store_true", help="Print stable JSON output.")
    return parser


def _task_report(task_id: str) -> dict[str, object]:
    registry = create_default_registry()
    registration = registry.resolve(task_id)
    config = build_run_config(task_id)
    registry.create_task(task_id, config.task)
    runtime = robot_runtime_from_config(config)
    return {
        "task_id": config.task_id,
        "task_version": config.task_version,
        "environment_id": registration.environment_id,
        "robot_id": registration.robot_id,
        "map_path": config.session.map_path,
        "slot_count": config.worker.slot_count,
        "device": config.runner.device,
        "max_iterations": config.runner.max_iterations,
        "actuator_count": len(runtime.actuators),
        "state_requirements": list(config.task.state_requirements),
        "action_schema": list(config.task.action_schema),
        "config_hash": config.normalized_hash,
    }


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.kind != "task":
        return 2
    try:
        report = _task_report(args.task_id)
    except RegistryError:
        candidates = suggest_task_ids(args.task_id)
        print(f"[FAIL] unknown Task {args.task_id!r}; run `uerl tasks` to list registered Tasks")
        if candidates:
            print(f"candidates: {', '.join(candidates)}")
        return 1
    except (ConfigError, UERLError, OSError) as exc:
        print(f"[FAIL] task preflight: {exc}")
        return 1
    if args.json:
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""List the explicitly supported Tasks without starting Unreal Engine."""

from __future__ import annotations

import argparse
import json

from ..tasks.registry import create_default_registry


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="List registered UE-RL Tasks.")
    parser.add_argument("--filter", help="Case-insensitive text filter over Task, robot, or environment IDs.")
    parser.add_argument("--json", action="store_true", help="Print stable JSON instead of a table.")
    return parser


def _robot_asset_name(asset_path: str) -> str:
    """Return the UE asset name that identifies one robot to a reader."""

    return asset_path.rsplit("/", 1)[-1].split(".", 1)[0]


def _task_rows(filter_text: str | None = None) -> list[dict[str, object]]:
    needle = filter_text.casefold() if filter_text else None
    rows: list[dict[str, object]] = []
    registry = create_default_registry()
    for registration in registry.list():
        worker = registry.create_worker_config(registration.task_id)
        searchable = " ".join(
            (
                registration.task_id,
                registration.robot_id,
                registration.environment_id,
                worker.robot_asset_path,
            )
        ).casefold()
        if needle is not None and needle not in searchable:
            continue
        runner = registry.create_runner_config(registration.task_id)
        rows.append(
            {
                "task_id": registration.task_id,
                "task_version": registration.task_version,
                "robot_id": registration.robot_id,
                "robot_asset_path": worker.robot_asset_path,
                "environment_id": registration.environment_id,
                "map_path": registration.session_config.map_path,
                "slot_count": worker.slot_count,
                "device": runner.device,
                "max_iterations": runner.max_iterations,
            }
        )
    return rows


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    rows = _task_rows(args.filter)
    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    if not rows:
        print("No registered Tasks matched the filter.")
        return 0
    print("TASK\tVERSION\tROBOT\tENVIRONMENT\tSLOTS\tDEVICE\tMAP")
    for row in rows:
        robot = _robot_asset_name(str(row["robot_asset_path"]))
        environment = str(row["environment_id"]).rsplit(".", 1)[-1]
        print(
            f"{row['task_id']}\t{row['task_version']}\t{robot}\t"
            f"{environment}\t{row['slot_count']}\t{row['device']}\t{row['map_path']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

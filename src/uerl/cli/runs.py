"""List training Runs and the checkpoint each one can resume from."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from ..training.runs import list_runs


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="List UE-RL training Runs without starting Unreal Engine.")
    parser.add_argument("--task", help="Keep Runs for this Task ID.")
    parser.add_argument("--root", type=Path, default=Path("runs"), help="Directory that contains Runs.")
    parser.add_argument("--json", action="store_true", help="Print stable JSON instead of a table.")
    return parser


def _checkpoint_label(run_directory: Path, checkpoint: Path | None) -> str | None:
    if checkpoint is None:
        return None
    try:
        return checkpoint.relative_to(run_directory).as_posix()
    except ValueError:
        return str(checkpoint)


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    rows = [
        {
            "task_id": run.task_id,
            "directory": str(run.directory),
            "checkpoint": _checkpoint_label(run.directory, run.checkpoint),
        }
        for run in list_runs(task_id=args.task, root=args.root)
    ]
    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=2, sort_keys=True))
        return 0
    if not rows:
        print(f"No Runs under {args.root}.")
        return 0
    print("TASK\tRUN\tCHECKPOINT")
    for row in rows:
        checkpoint = row["checkpoint"] or "-"
        print(f"{row['task_id']}\t{row['directory']}\t{checkpoint}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

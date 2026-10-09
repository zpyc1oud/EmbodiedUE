"""Report motion, tracking, actions and force observations from a Task YAML trace."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from uerl.training.trace_diagnostics import diagnose_task_trace, summarize_training_scalars  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--body", default="base_link")
    parser.add_argument("--command", default="velocity")
    parser.add_argument("--low-motion-speed", type=float, default=0.01)
    parser.add_argument("--events", type=Path, help="Optional Run TensorBoard directory.")
    parser.add_argument("--last-iterations", type=int, default=100)
    args = parser.parse_args(argv)
    try:
        report = diagnose_task_trace(
            args.trace, body=args.body, command=args.command, low_motion_speed=args.low_motion_speed
        )
        if args.events is not None:
            report["training_scalars"] = summarize_training_scalars(args.events, last_iterations=args.last_iterations)
    except (OSError, ValueError, TypeError, yaml.YAMLError) as exc:
        print(f"Trace diagnosis failed: {exc}", file=sys.stderr)
        return 2
    print(yaml.safe_dump(report, sort_keys=False), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

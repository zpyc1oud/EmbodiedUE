"""Inspect training debug captures and policy/task YAML traces."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path


def _add_training_debug_args(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    parser.add_argument("trace", type=Path, help="Bounded all-Slot debug trace YAML file.")
    return parser


def _add_compare_traces_args(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    parser.add_argument("task_trace", type=Path, help="YAML written by `uerl play --trace`.")
    parser.add_argument("ue_trace", type=Path, help="YAML written by the UE host trace recorder.")
    return parser


def _add_training_trace_args(parser: argparse.ArgumentParser) -> argparse.ArgumentParser:
    parser.add_argument("trace", type=Path, help="Task trace YAML written by `uerl play --trace`.")
    parser.add_argument("--body", default="base_link")
    parser.add_argument("--command", default="velocity")
    parser.add_argument("--low-motion-speed", type=float, default=0.01)
    parser.add_argument("--events", type=Path, help="Optional Run TensorBoard directory.")
    parser.add_argument("--last-iterations", type=int, default=100)
    return parser


def _exec_training_debug(args: argparse.Namespace) -> int:
    import yaml

    from ..training.debug_audit import audit_training_trace

    print(yaml.safe_dump(audit_training_trace(args.trace), sort_keys=False), end="")
    return 0


def _exec_compare_traces(args: argparse.Namespace) -> int:
    import yaml

    from ..training.policy_trace import compare_policy_traces

    try:
        report = compare_policy_traces(args.task_trace, args.ue_trace)
    except (OSError, TypeError, KeyError, IndexError, ValueError) as exc:
        print(f"Trace comparison failed: {exc}", file=sys.stderr)
        return 2
    print(yaml.safe_dump(report, sort_keys=False, allow_unicode=True), end="")
    return 0 if report["status"] == "comparable" else 2


def _exec_training_trace(args: argparse.Namespace) -> int:
    import yaml

    from ..training.trace_diagnostics import diagnose_task_trace, summarize_training_scalars

    try:
        report = diagnose_task_trace(
            args.trace,
            body=args.body,
            command=args.command,
            low_motion_speed=args.low_motion_speed,
        )
        if args.events is not None:
            report["training_scalars"] = summarize_training_scalars(
                args.events,
                last_iterations=args.last_iterations,
            )
    except (OSError, ValueError, TypeError, yaml.YAMLError) as exc:
        print(f"Trace diagnosis failed: {exc}", file=sys.stderr)
        return 2
    print(yaml.safe_dump(report, sort_keys=False), end="")
    return 0


def run_training_debug(argv: list[str] | None = None) -> int:
    """Standalone entry point for ``check_training_debug.py``."""

    parser = _add_training_debug_args(
        argparse.ArgumentParser(description="Check a bounded all-Slot training capture without starting UE.")
    )
    return _exec_training_debug(parser.parse_args(argv))


def run_compare_traces(argv: list[str] | None = None) -> int:
    """Standalone entry point for ``compare_policy_traces.py``."""

    parser = _add_compare_traces_args(
        argparse.ArgumentParser(description="Compare saved Task and Unreal Engine policy-frame YAML traces.")
    )
    return _exec_compare_traces(parser.parse_args(argv))


def run_training_trace(argv: list[str] | None = None) -> int:
    """Standalone entry point for ``diagnose_training_trace.py``."""

    parser = _add_training_trace_args(
        argparse.ArgumentParser(
            description="Report motion, tracking, actions and force observations from a Task YAML trace."
        )
    )
    return _exec_training_trace(parser.parse_args(argv))


_SUBCOMMANDS: dict[str, Callable[[argparse.Namespace], int]] = {
    "training-debug": _exec_training_debug,
    "compare-traces": _exec_compare_traces,
    "training-trace": _exec_training_trace,
}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="uerl diagnose",
        description="Inspect offline training debug captures and recorded policy/task traces.",
    )
    subparsers = parser.add_subparsers(dest="subcommand", required=True)
    _add_training_debug_args(
        subparsers.add_parser(
            "training-debug",
            help="Check a bounded all-Slot training capture without starting UE.",
        )
    )
    _add_compare_traces_args(
        subparsers.add_parser(
            "compare-traces",
            help="Compare saved Task and Unreal Engine policy-frame YAML traces.",
        )
    )
    _add_training_trace_args(
        subparsers.add_parser(
            "training-trace",
            help="Report motion, tracking, actions and force observations from a Task YAML trace.",
        )
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """Dispatch ``uerl diagnose <subcommand>``."""

    args = _parser().parse_args(argv)
    return _SUBCOMMANDS[args.subcommand](args)


if __name__ == "__main__":
    raise SystemExit(main())

"""Dispatch the discoverable ``uerl <command>`` command family."""

from __future__ import annotations

import argparse
import sys

from . import check, config, deploy, export, new, play, runs, tasks, train

_COMMANDS = {
    "check": check.main,
    "config": config.main,
    "deploy": deploy.main,
    "export": export.main,
    "new": new.main,
    "play": play.main,
    "runs": runs.main,
    "tasks": tasks.main,
    "train": train.main,
}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="uerl",
        description="Discoverable UE-RL training, playback, export, and inspection commands.",
    )
    parser.add_argument("command", choices=tuple(_COMMANDS), help="Command to run.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in {"-h", "--help"}:
        _parser().print_help()
        return 0
    command = args.pop(0)
    if command not in _COMMANDS:
        parser = _parser()
        parser.error(f"unknown command: {command}")
    return _COMMANDS[command](args)


if __name__ == "__main__":
    raise SystemExit(main())

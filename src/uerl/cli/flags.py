"""Map everyday training flags onto the typed config paths they set."""

from __future__ import annotations

import argparse

_FLAG_PATHS = {
    "num_envs": "worker.slot_count",
    "seed": "worker.run_seed",
    "device": "runner.device",
    "max_iterations": "runner.max_iterations",
}
_FLAG_HELP = {
    "num_envs": "Parallel Slot count (worker.slot_count).",
    "seed": "Run seed (worker.run_seed).",
    "device": "Policy device, for example cuda:0 or cpu (runner.device).",
    "max_iterations": "Training iterations (runner.max_iterations).",
}
TRAIN_FLAGS = ("num_envs", "seed", "device", "max_iterations")
PLAY_FLAGS = ("seed", "device")
EXPORT_FLAGS = ("device",)


def add_common_flags(parser: argparse.ArgumentParser, names: tuple[str, ...]) -> None:
    """Add the everyday flags selected by one command."""

    for name in names:
        flag = f"--{name.replace('_', '-')}"
        if name == "device":
            parser.add_argument(flag, dest=name, help=_FLAG_HELP[name])
        else:
            parser.add_argument(flag, dest=name, type=int, help=_FLAG_HELP[name])


def apply_common_flags(
    parser: argparse.ArgumentParser,
    args: argparse.Namespace,
    overrides: dict[str, str],
    names: tuple[str, ...],
) -> dict[str, str]:
    """Copy set flags into overrides, rejecting a dotted path for the same field."""

    merged = dict(overrides)
    for name in names:
        value = getattr(args, name)
        if value is None:
            continue
        path = _FLAG_PATHS[name]
        flag = f"--{name.replace('_', '-')}"
        if path in merged:
            parser.error(f"{flag} cannot be combined with --{path}")
        merged[path] = str(value)
    return merged


__all__ = [
    "EXPORT_FLAGS",
    "PLAY_FLAGS",
    "TRAIN_FLAGS",
    "add_common_flags",
    "apply_common_flags",
]

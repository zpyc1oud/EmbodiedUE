"""Preview one resolved Task configuration without starting Unreal Engine."""

from __future__ import annotations

import argparse

from ..core.config.canonical import canonical_json, to_jsonable
from ..training import build_run_config
from .boundary import guard, parse_overrides


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Resolve and print one UE-RL Task configuration without starting Unreal Engine."
    )
    parser.add_argument("--task", required=True, help="Registered Task ID.")
    parser.add_argument("--json", action="store_true", help="Print the complete resolved config as JSON.")
    return parser


@guard
def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args, remaining = parser.parse_known_args(argv)
    config = build_run_config(args.task, overrides=parse_overrides(parser, remaining))
    if args.json:
        print(canonical_json(config))
        return 0
    values = to_jsonable(config)
    print(f"task={values['task_id']} version={values['task_version']} config_hash={values['normalized_hash']}")
    print(
        f"map={values['session']['map_path']} slots={values['worker']['slot_count']} "
        f"device={values['runner']['device']}"
    )
    print(canonical_json(config))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

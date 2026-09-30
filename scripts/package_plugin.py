"""Print or run UAT BuildPlugin for the UERLEngine Win64 editor binaries."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

DEFAULT_ENGINE = Path(r"C:\Program Files\Epic Games\UE_5.8")


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_engine(explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit.resolve()
    env = os.environ.get("UE_ROOT") or os.environ.get("UE_58_ROOT")
    if env:
        return Path(env).resolve()
    return DEFAULT_ENGINE


def build_uat_command(*, engine_root: Path, repo_root: Path, output: Path) -> list[str]:
    uat = engine_root / "Engine" / "Build" / "BatchFiles" / "RunUAT.bat"
    plugin = repo_root / "engine" / "Plugins" / "UERLEngine" / "UERLEngine.uplugin"
    return [
        str(uat),
        "BuildPlugin",
        f"-Plugin={plugin}",
        f"-Package={output.resolve()}",
        "-TargetPlatforms=Win64",
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="UAT -Package output directory")
    parser.add_argument(
        "--engine",
        type=Path,
        default=None,
        help="UE 5.8 root (defaults to UE_ROOT or Epic install path)",
    )
    parser.add_argument("--repo-root", type=Path, default=None, help="repository root that owns UERLEngine.uplugin")
    parser.add_argument("--dry-run", action="store_true", help="print the UAT command without running it")
    args = parser.parse_args(argv)
    repo_root = args.repo_root.resolve() if args.repo_root is not None else repo_root_from_script()
    engine_root = resolve_engine(args.engine)
    command = build_uat_command(engine_root=engine_root, repo_root=repo_root, output=args.output)
    printable = subprocess.list2cmdline(command)
    print(printable)
    if args.dry_run:
        return 0
    uat = Path(command[0])
    if not uat.is_file():
        print(f"RunUAT.bat not found: {uat}", file=sys.stderr)
        return 1
    completed = subprocess.run(command, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())

"""Share machine profile flags across host checks and runtime commands."""

from __future__ import annotations

import argparse
from pathlib import Path

from ..host import HostProfile, resolve_host_profile


def add_host_flags(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host-profile", type=Path, help="TOML profile; else UERL_HOST_PROFILE or ~/.uerl/host.toml.")
    parser.add_argument("--ue-executable", type=Path, help="Override the profile's UnrealEditor-Cmd.exe path.")
    parser.add_argument("--project", type=Path, help="Override the profile's host .uproject path.")


def resolve_host_flags(args: argparse.Namespace) -> HostProfile:
    """Resolve machine paths only when the caller needs a local host."""
    return resolve_host_profile(
        profile_path=args.host_profile,
        ue_executable=args.ue_executable,
        project=args.project,
    )

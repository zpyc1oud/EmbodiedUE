"""Present static CartPole host diagnostics without opening a Session."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict

from ..errors import ConfigError
from ..host import check_host
from .host_flags import add_host_flags, resolve_host_flags


def add_host_arguments(parser: argparse.ArgumentParser) -> None:
    add_host_flags(parser)
    parser.add_argument("--json", action="store_true", help="Print static checks and selected paths as JSON.")


def run_host_check(args: argparse.Namespace) -> int:
    try:
        profile = resolve_host_flags(args)
    except ConfigError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": {"code": exc.code, "message": str(exc)}}))
        else:
            print(f"[FAIL] {exc.code}: {exc}")
        return 1
    report = check_host(profile)
    selected = {
        "ue_executable": str(profile.ue_executable),
        "project": str(profile.project),
        "profile_path": str(profile.profile_path),
        "sources": profile.sources,
    }
    scope = (
        "Static filesystem checks for the bundled Windows/UE 5.8 CartPole host only. "
        "No UE process was launched. Asset loading, build compatibility, devices and runtime readiness are unverified."
    )
    if args.json:
        print(
            json.dumps(
                {
                    "ok": report.ok,
                    "scope": scope,
                    "platform": sys.platform,
                    "selected": selected,
                    "checks": [asdict(check) for check in report.checks],
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print(scope)
        print(f"platform={sys.platform} profile={profile.profile_path}")
        for field in ("ue_executable", "project"):
            print(f"{field}={selected[field]} source={profile.sources[field]}")
        for check in report.checks:
            print(f"[{'PASS' if check.ok else 'FAIL'}] {check.code}: {check.path}: {check.message}")
            if check.action:
                print(f"  action: {check.action}")
    return 0 if report.ok else 1

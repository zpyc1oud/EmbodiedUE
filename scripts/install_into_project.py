"""Thin CLI wrapper delegating to ``uerl.application.project_installer``."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_SRC_ROOT = Path(__file__).resolve().parents[1] / "src"
if str(_SRC_ROOT) not in sys.path:
    sys.path.insert(0, str(_SRC_ROOT))

from uerl.application.project_installer import (  # noqa: E402
    DEFAULT_MAX_PHYSICS_DELTA,
    DEFAULT_MAX_SUBSTEP_DELTA,
    DEFAULT_MAX_SUBSTEPS,
    DEMO_ROBOT_ASSETS,
    PHYSICS_BOOLS,
    PHYSICS_SECTION,
    PLUGIN_COPY_IGNORE,
    PLUGIN_NAME,
    REQUIRED_PLUGINS,
    InstallError,
    PreflightCheck,
    check_project,
    copy_demo_robot,
    copy_plugin,
    enable_plugins,
    install,
    module_name_from_uproject,
    resolve_project,
    scaffold_game_module,
    upsert_physics_ini,
)


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-dir", required=True, type=Path, help="target UE project directory or .uproject")
    parser.add_argument(
        "--force",
        action="store_true",
        help="overwrite an existing plugin copy and selected demo assets",
    )
    parser.add_argument(
        "--demo",
        choices=("phantomx",),
        help="install optional demo assets; omit for a generic runtime-only install",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="read-only deployment preflight; does not modify the project",
    )
    parser.add_argument(
        "--artifact",
        type=Path,
        help="optional .uerlpol2 artifact to validate during --check",
    )
    parser.add_argument(
        "--from-package",
        type=Path,
        default=None,
        help="prebuilt plugin directory from package_plugin.py instead of this repo's source plugin",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="repository root that owns engine/Plugins/UERLEngine (defaults to this script's repo)",
    )
    args = parser.parse_args(argv)
    repo_root = args.repo_root.resolve() if args.repo_root is not None else repo_root_from_script()
    try:
        if args.check:
            if args.force or args.demo is not None or args.from_package is not None:
                parser.error("--check cannot be combined with install/write options")
            checks = check_project(project_dir=args.project_dir, artifact=args.artifact)
            for check in checks:
                status = "PASS" if check.passed else "FAIL"
                print(f"[{status}] {check.name}: {check.detail}")
            return 0 if all(check.passed for check in checks) else 1
        if args.artifact is not None:
            parser.error("--artifact requires --check")
        for line in install(
            project_dir=args.project_dir,
            repo_root=repo_root,
            force=args.force,
            from_package=args.from_package,
            demo=args.demo,
        ):
            print(line)
    except InstallError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


__all__ = [
    "DEFAULT_MAX_PHYSICS_DELTA",
    "DEFAULT_MAX_SUBSTEPS",
    "DEFAULT_MAX_SUBSTEP_DELTA",
    "DEMO_ROBOT_ASSETS",
    "InstallError",
    "PHYSICS_BOOLS",
    "PHYSICS_SECTION",
    "PLUGIN_COPY_IGNORE",
    "PLUGIN_NAME",
    "PreflightCheck",
    "REQUIRED_PLUGINS",
    "check_project",
    "copy_demo_robot",
    "copy_plugin",
    "enable_plugins",
    "install",
    "main",
    "module_name_from_uproject",
    "repo_root_from_script",
    "resolve_project",
    "scaffold_game_module",
    "upsert_physics_ini",
]


if __name__ == "__main__":
    raise SystemExit(main())

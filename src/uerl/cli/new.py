"""Generate reviewable skeletons for new Robot declarations and Tasks."""

from __future__ import annotations

import argparse
import json
import keyword
import re
from pathlib import Path
from typing import cast

from .new_templates import (
    TASK_TEMPLATES as _TASK_TEMPLATES,
)
from .new_templates import (
    ScaffoldError as _ScaffoldError,
)
from .new_templates import (
    build_direct_cartpole_files as _direct_cartpole_files,
)
from .new_templates import (
    build_external_cartpole_files as _external_cartpole_files,
)
from .new_templates import (
    build_robot_files as _robot_files,
)
from .new_templates import (
    build_task_files as _task_files,
)
from .new_templates import (
    default_task_id as _task_id,
)
from .new_templates import (
    slugify as _slug,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="uerl new", description="Generate UE-RL Robot and Task projects.")
    subparsers = parser.add_subparsers(dest="kind", required=True)

    robot_parser = subparsers.add_parser("robot", help="generate a Robot declaration skeleton")
    robot_parser.add_argument("name", help="Python Robot declaration name, such as phantomx2.")
    robot_parser.add_argument("--asset", required=True, help="UE Skeletal Mesh object path.")
    robot_parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("."),
        help="Project root where src/ and docs/ are created (default: current directory).",
    )
    robot_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")

    task_parser = subparsers.add_parser("task", help="generate a DirectTask skeleton")
    task_parser.add_argument("name", help="Python Task package name, such as walk2.")
    task_parser.add_argument("--robot", required=True, help="Robot declaration name or slug.")
    task_parser.add_argument(
        "--template",
        required=True,
        choices=tuple(sorted(_TASK_TEMPLATES)),
        help="Supported starting point for the Task math and config draft.",
    )
    task_parser.add_argument("--task-id", help="Stable Task ID (default: UERL-<Name>-v0).")
    task_parser.add_argument(
        "--environment-id",
        help="UE Environment ID (default comes from the selected template).",
    )
    task_parser.add_argument(
        "--robot-id",
        default="uerl.robot.skeletal_mesh",
        help="UE Robot ID used by the Task registration draft.",
    )
    task_parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("."),
        help="Project root where src/uerl/ and docs/ are created (default: current directory).",
    )
    task_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")

    cartpole_parser = subparsers.add_parser(
        "external-cartpole",
        help="generate an independently installable CartPole Task package",
    )
    cartpole_parser.add_argument("name", help="Package name, such as balance-demo.")
    cartpole_parser.add_argument("--task-id", help="Stable Task ID (default: UERL-<Name>-v0).")
    cartpole_parser.add_argument(
        "--output-dir",
        type=Path,
        help="Destination project directory (default: <name> under the current directory).",
    )
    cartpole_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")

    direct_cartpole_parser = subparsers.add_parser(
        "direct-cartpole",
        help="generate an independently installable CartPole Task using Python hooks",
    )
    direct_cartpole_parser.add_argument("name", help="Package name, such as direct-balance-demo.")
    direct_cartpole_parser.add_argument("--task-id", help="Stable Task ID (default: UERL-<Name>-v0).")
    direct_cartpole_parser.add_argument(
        "--output-dir",
        type=Path,
        help="Destination project directory (default: <name> under the current directory).",
    )
    direct_cartpole_parser.add_argument("--json", action="store_true", help="Print a machine-readable result.")
    return parser


def _write_files(files: dict[Path, str]) -> tuple[Path, ...]:
    """Write a complete scaffold only after every destination is confirmed free."""

    conflicts = sorted(path for path in files if path.exists())
    if conflicts:
        rendered = ", ".join(str(path) for path in conflicts)
        raise _ScaffoldError(f"refusing to overwrite existing scaffold file(s): {rendered}")
    for path, content in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
    return tuple(files)


def _emit(result: dict[str, object], *, as_json: bool) -> None:
    if as_json:
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
        return
    print(f"[PASS] generated {result['kind']} scaffold: {result['name']}")
    for path in cast(tuple[Path, ...], result["files"]):
        print(f"[WRITE] {path}")
    print(f"[NEXT] {result['next_step']}")


def _generate_robot(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="robot name")
    if not args.asset.startswith("/") or "\\" in args.asset:
        raise _ScaffoldError("--asset must be a UE object path beginning with '/' and must not contain '\\'")
    files = _write_files(_robot_files(args.output_dir, slug, args.asset))
    return {
        "kind": "robot",
        "name": slug,
        "files": [str(path) for path in files],
        "next_step": "replace TODO selectors, then add the declaration to ROBOT_ASSETS",
    }


def _generate_task(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="task name")
    robot_slug = _slug(args.robot, label="robot name")
    template = _TASK_TEMPLATES[args.template]
    task_id = args.task_id or _task_id(slug)
    if not re.fullmatch(r"UERL-[A-Za-z0-9][A-Za-z0-9._-]*-v\d+", task_id):
        raise _ScaffoldError("--task-id must look like UERL-Name-v0")
    environment_id = args.environment_id or template.default_environment_id
    if not environment_id or " " in environment_id:
        raise _ScaffoldError("--environment-id must be a non-empty identifier")
    if not args.robot_id or " " in args.robot_id:
        raise _ScaffoldError("--robot-id must be a non-empty identifier")
    files = _write_files(
        _task_files(
            args.output_dir,
            slug,
            robot_slug,
            template,
            task_id,
            environment_id,
            args.robot_id,
        )
    )
    return {
        "kind": "task",
        "name": slug,
        "task_id": task_id,
        "template": template.name,
        "files": [str(path) for path in files],
        "next_step": f"implement and register the Task, then run `uerl check task {task_id}`",
    }


def _generate_external_cartpole(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="package name")
    if keyword.iskeyword(slug):
        raise _ScaffoldError("package name must not be a Python keyword")
    task_id = args.task_id or _task_id(slug)
    if not re.fullmatch(r"UERL-[A-Za-z0-9][A-Za-z0-9._-]*-v\d+", task_id):
        raise _ScaffoldError("--task-id must look like UERL-Name-v0")
    root = args.output_dir if args.output_dir is not None else Path(slug.replace("_", "-"))
    files = _write_files(_external_cartpole_files(root, slug, task_id))
    return {
        "kind": "external-cartpole",
        "name": slug,
        "package_name": slug,
        "distribution_name": f"uerl-{slug.replace('_', '-')}",
        "entry_point": slug.replace("_", "-"),
        "task_id": task_id,
        "files": [str(path) for path in files],
        "next_step": f"install the package, enable its entry point, then run `uerl check task {task_id}`",
    }


def _generate_direct_cartpole(args: argparse.Namespace) -> dict[str, object]:
    slug = _slug(args.name, label="package name")
    if keyword.iskeyword(slug):
        raise _ScaffoldError("package name must not be a Python keyword")
    task_id = args.task_id or _task_id(slug)
    if not re.fullmatch(r"UERL-[A-Za-z0-9][A-Za-z0-9._-]*-v\d+", task_id):
        raise _ScaffoldError("--task-id must look like UERL-Name-v0")
    root = args.output_dir if args.output_dir is not None else Path(slug.replace("_", "-"))
    files = _write_files(_direct_cartpole_files(root, slug, task_id))
    return {
        "kind": "direct-cartpole",
        "name": slug,
        "package_name": slug,
        "distribution_name": f"uerl-{slug.replace('_', '-')}-direct",
        "entry_point": slug.replace("_", "-"),
        "task_id": task_id,
        "files": [str(path) for path in files],
        "next_step": f"install the package, enable its entry point, then run `uerl check task {task_id}`",
    }


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.kind == "robot":
            result = _generate_robot(args)
        elif args.kind == "task":
            result = _generate_task(args)
        elif args.kind == "direct-cartpole":
            result = _generate_direct_cartpole(args)
        else:
            result = _generate_external_cartpole(args)
    except (_ScaffoldError, OSError) as exc:
        print(f"[FAIL] scaffold: {exc}")
        return 1
    _emit(result, as_json=args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

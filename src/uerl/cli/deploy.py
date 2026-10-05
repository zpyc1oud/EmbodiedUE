"""Install the runtime into a UE project and print the policy import command."""

from __future__ import annotations

import argparse
import importlib.util
import re
import subprocess
import sys
from pathlib import Path
from types import ModuleType

from ..application.run_config import RESOLVED_CONFIG_FILENAME
from ..core.config.snapshot import resolved_config_from_yaml
from ..errors import ConfigError
from ..training.runs import resolve_run_directory
from .boundary import guard

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_UE_EDITOR = Path(r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe")
_ASSET_NAME = re.compile(r"[^A-Za-z0-9_]+")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Install UERLEngine into a UE project and prepare one .uerlpol2 import."
    )
    parser.add_argument("--project", required=True, type=Path, help="Target project directory or .uproject.")
    parser.add_argument("--demo", choices=("phantomx",), help="Also install the PhantomX demo assets.")
    parser.add_argument("--force", action="store_true", help="Overwrite an existing plugin copy and demo assets.")
    parser.add_argument("--from-package", type=Path, help="Prebuilt plugin directory instead of this repo's plugin.")
    parser.add_argument("--check", action="store_true", help="Read-only preflight; does not modify the project.")
    parser.add_argument(
        "--artifact",
        help="A .uerlpol2 file, a Run directory, or 'latest'. 'latest' requires --task.",
    )
    parser.add_argument("--task", help="Task ID used to resolve 'latest' and the default RobotMesh.")
    parser.add_argument("--asset", help="UE package for the imported policy. Defaults from --task.")
    parser.add_argument("--robotmesh", help="UE mesh package. Defaults from --task or the Run's resolved config.")
    parser.add_argument(
        "--replace-existing",
        action="store_true",
        help="Pass -replaceexisting so the commandlet updates an asset that already exists.",
    )
    parser.add_argument(
        "--import",
        dest="import_artifact",
        action="store_true",
        help="Run the UERLPolicyImport commandlet after preparing the project.",
    )
    parser.add_argument("--ue-executable", type=Path, default=DEFAULT_UE_EDITOR, help="Path to UnrealEditor.exe.")
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT, help="Repository that owns the source plugin.")
    return parser


def _installer() -> ModuleType:
    script = REPO_ROOT / "scripts" / "install_into_project.py"
    spec = importlib.util.spec_from_file_location("install_into_project", script)
    if spec is None or spec.loader is None:
        raise FileNotFoundError(f"install script not found: {script}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _resolve_artifact(reference: str, task_id: str | None) -> tuple[Path, Path | None]:
    """Return ``(artifact, run_directory)``. A bare file has no Run directory."""

    path = Path(reference)
    if reference != "latest" and path.is_file():
        run_directory = path.parent.parent if path.parent.name == "exported" else None
        return path, run_directory
    run_directory = resolve_run_directory(reference, task_id=task_id)
    return _artifact_in_run(run_directory, task_id), run_directory


def _artifact_in_run(run_directory: Path, task_id: str | None) -> Path:
    exported = run_directory / "exported"
    if task_id:
        preferred = exported / f"{task_id}.uerlpol2"
        if preferred.is_file():
            return preferred
    files = sorted(exported.glob("*.uerlpol2")) if exported.is_dir() else []
    if len(files) == 1:
        return files[0]
    if len(files) > 1:
        names = ", ".join(path.name for path in files)
        raise FileNotFoundError(f"multiple artifacts in {exported}: {names}; pass --task or the file")
    raise FileNotFoundError(f"no .uerlpol2 under {exported}")


def _package_path(asset_path: str) -> str:
    package, dot, asset = asset_path.rpartition(".")
    if dot and package.startswith("/") and package.rsplit("/", 1)[-1] == asset:
        return package
    return asset_path


def _mesh_from_task(task_id: str) -> str:
    from ..training import build_run_config

    return _package_path(build_run_config(task_id).worker.robot_asset_path)


def _mesh_from_run(run_directory: Path | None) -> str | None:
    if run_directory is None:
        return None
    path = run_directory / RESOLVED_CONFIG_FILENAME
    try:
        text = path.read_text(encoding="utf-8")
        payload = resolved_config_from_yaml(text, path=str(path))
    except (OSError, UnicodeDecodeError, ConfigError, TypeError, ValueError):
        return None
    worker = payload.get("worker") if isinstance(payload, dict) else None
    asset = worker.get("robot_asset_path") if isinstance(worker, dict) else None
    if not isinstance(asset, str) or not asset:
        return None
    return _package_path(asset)


def _policy_asset(task_id: str) -> str:
    name = _ASSET_NAME.sub("", task_id) or "Policy"
    return f"/Game/UERLEngine/Policies/{name}"


def _import_command(
    *,
    ue_executable: Path,
    uproject: Path,
    artifact: Path,
    asset: str,
    robotmesh: str,
    replace_existing: bool,
) -> list[str]:
    command = [
        str(ue_executable),
        str(uproject),
        "-run=UERLPolicyImport",
        f"-artifact={artifact}",
        f"-asset={asset}",
        f"-robotmesh={robotmesh}",
        "-unattended",
        "-nop4",
    ]
    if replace_existing:
        command.append("-replaceexisting")
    return command


@guard
def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    if args.check and (args.force or args.demo is not None or args.from_package is not None or args.import_artifact):
        parser.error("--check cannot be combined with install or --import")
    if args.import_artifact and args.artifact is None:
        parser.error("--import requires --artifact")
    artifact: Path | None = None
    run_directory: Path | None = None
    robotmesh = args.robotmesh
    asset = args.asset
    if args.artifact is not None:
        artifact, run_directory = _resolve_artifact(args.artifact, args.task)
        if robotmesh is None and args.task:
            robotmesh = _mesh_from_task(args.task)
        if robotmesh is None:
            robotmesh = _mesh_from_run(run_directory)
        if asset is None and args.task:
            asset = _policy_asset(args.task)
        if args.import_artifact and (not robotmesh or not asset):
            print("[FAIL] pass --task or both --asset and --robotmesh")
            return 1

    installer = _installer()
    try:
        if args.check:
            checks = installer.check_project(project_dir=args.project, artifact=artifact)
            for check in checks:
                status = "PASS" if check.passed else "FAIL"
                print(f"[{status}] {check.name}: {check.detail}")
            if not all(check.passed for check in checks):
                return 1
        else:
            for line in installer.install(
                project_dir=args.project,
                repo_root=args.repo_root.resolve(),
                force=args.force,
                from_package=args.from_package,
                demo=args.demo,
            ):
                print(line)
        if artifact is None:
            print("[NEXT] open the project, compile if a game module was just added, then run UERL.CheckProject")
            return 0
        if not robotmesh or not asset:
            print("[NEXT] pass --task or both --asset and --robotmesh to print the import command")
            return 1 if args.import_artifact else 0
        project_root, uproject = installer.resolve_project(args.project)
        del project_root
        command = _import_command(
            ue_executable=args.ue_executable,
            uproject=uproject,
            artifact=artifact,
            asset=asset,
            robotmesh=robotmesh,
            replace_existing=args.replace_existing,
        )
        print(f"[NEXT] {subprocess.list2cmdline(command)}")
        if not args.import_artifact:
            print("[NEXT] add --import to run that command, then run UERL.CheckProject and place BP_UERLPolicyRobot")
            return 0
        if not args.ue_executable.is_file():
            print(f"[FAIL] Unreal Editor not found: {args.ue_executable}")
            return 1
        return subprocess.run(command, check=False).returncode
    except installer.InstallError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

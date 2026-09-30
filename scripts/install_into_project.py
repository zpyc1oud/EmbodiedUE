"""Install UERLEngine runtime and optional demo assets into a UE project."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path
from typing import Any


class InstallError(Exception):
    """User-facing failure while installing into a target project."""


PLUGIN_NAME = "UERLEngine"
REQUIRED_PLUGINS = ("UERLEngine", "ProceduralMeshComponent", "NNERuntimeORT")
PLUGIN_COPY_IGNORE = {"Binaries", "Intermediate", "Saved", "DerivedDataCache", ".vs"}
DEMO_ROBOT_ASSETS = ("SK_PhantomX.uasset", "SK_PhantomX_Skeleton.uasset", "PA_PhantomX.uasset")
PHYSICS_SECTION = "[/Script/Engine.PhysicsSettings]"
PHYSICS_BOOLS = {
    "bTickPhysicsAsync": "False",
    "bSubstepping": "True",
    "bSubsteppingAsync": "False",
}
DEFAULT_MAX_SUBSTEP_DELTA = 0.005
DEFAULT_MAX_SUBSTEPS = 7
DEFAULT_MAX_PHYSICS_DELTA = "0.033333"


class PreflightCheck:
    """One read-only deployment preflight result."""

    __slots__ = ("name", "passed", "detail")

    def __init__(self, name: str, passed: bool, detail: str) -> None:
        self.name = name
        self.passed = passed
        self.detail = detail


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_project(project_dir: Path) -> tuple[Path, Path]:
    """Return ``(project_root, uproject_path)``."""
    project_dir = project_dir.resolve()
    if project_dir.is_file() and project_dir.suffix.lower() == ".uproject":
        return project_dir.parent, project_dir
    if not project_dir.is_dir():
        raise InstallError(f"project directory does not exist: {project_dir}")
    uprojects = sorted(project_dir.glob("*.uproject"))
    if len(uprojects) != 1:
        raise InstallError(f"expected exactly one .uproject in {project_dir}, found {len(uprojects)}")
    return project_dir, uprojects[0]


def _copy_tree(source: Path, destination: Path, force: bool, ignore: set[str]) -> str:
    if not source.is_dir():
        raise InstallError(f"plugin source does not exist: {source}")
    if destination.exists() and not force:
        return f"skipped plugin (already present, use --force to overwrite): {destination}"
    if destination.exists() and force:
        shutil.rmtree(destination)
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns(*ignore) if ignore else None)
    return f"copied plugin -> {destination}"


def copy_plugin(*, repo_root: Path, project_root: Path, from_package: Path | None, force: bool) -> str:
    source = from_package.resolve() if from_package is not None else repo_root / "engine" / "Plugins" / PLUGIN_NAME
    destination = project_root / "Plugins" / PLUGIN_NAME
    ignore = set() if from_package is not None else PLUGIN_COPY_IGNORE
    return _copy_tree(source, destination, force, ignore)


def copy_demo_robot(*, repo_root: Path, project_root: Path, force: bool) -> list[str]:
    source_dir = repo_root / "engine" / "Content" / "Robots" / "PhantomX"
    target_dir = project_root / "Content" / "Robots" / "PhantomX"
    target_dir.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    for name in DEMO_ROBOT_ASSETS:
        source = source_dir / name
        if not source.is_file():
            raise InstallError(f"repo demo asset is missing: {source}")
        target = target_dir / name
        if target.exists() and not force:
            lines.append(f"skipped {name} (already present, use --force to overwrite)")
            continue
        shutil.copy2(source, target)
        lines.append(f"copied {name}")
    return lines


def enable_plugins(uproject_path: Path) -> str:
    data = json.loads(uproject_path.read_text(encoding="utf-8-sig"))
    plugins: list[dict[str, Any]] = list(data.get("Plugins") or [])
    by_name = {str(entry.get("Name")): entry for entry in plugins if isinstance(entry, dict)}
    changed = False
    for name in REQUIRED_PLUGINS:
        existing = by_name.get(name)
        if existing is None:
            plugins.append({"Name": name, "Enabled": True})
            by_name[name] = plugins[-1]
            changed = True
            continue
        if existing.get("Enabled") is not True:
            existing["Enabled"] = True
            changed = True
    data["Plugins"] = plugins
    if changed:
        uproject_path.write_text(json.dumps(data, indent="\t", ensure_ascii=False) + "\n", encoding="utf-8")
        return f"enabled plugins in {uproject_path.name}: {', '.join(REQUIRED_PLUGINS)}"
    return f"plugins already enabled in {uproject_path.name}: {', '.join(REQUIRED_PLUGINS)}"


def _parse_sections(text: str) -> list[tuple[str | None, list[str]]]:
    sections: list[tuple[str | None, list[str]]] = [(None, [])]
    for raw_line in text.splitlines():
        stripped = raw_line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            sections.append((stripped, []))
            continue
        sections[-1][1].append(raw_line)
    return sections


def _set_key(lines: list[str], key: str, value: str) -> None:
    prefix = f"{key}="
    replacement = f"{key}={value}"
    for index, line in enumerate(lines):
        if line.strip().startswith(prefix):
            lines[index] = replacement
            return
    while lines and lines[-1].strip() == "":
        lines.pop()
    lines.append(replacement)


def _get_key(lines: list[str], key: str) -> str | None:
    prefix = f"{key}="
    for line in lines:
        stripped = line.strip()
        if stripped.startswith(prefix):
            return stripped[len(prefix) :]
    return None


def upsert_physics_ini(ini_path: Path) -> str:
    ini_path.parent.mkdir(parents=True, exist_ok=True)
    text = ini_path.read_text(encoding="utf-8") if ini_path.exists() else ""
    sections = _parse_sections(text)
    physics_lines: list[str] | None = None
    for header, lines in sections:
        if header == PHYSICS_SECTION:
            physics_lines = lines
            break
    created = physics_lines is None
    if physics_lines is None:
        physics_lines = []
        sections.append((PHYSICS_SECTION, physics_lines))

    for key, value in PHYSICS_BOOLS.items():
        _set_key(physics_lines, key, value)

    existing_dt = _get_key(physics_lines, "MaxSubstepDeltaTime")
    try:
        dt_ok = existing_dt is not None and float(existing_dt) <= DEFAULT_MAX_SUBSTEP_DELTA
    except ValueError:
        dt_ok = False
    if not dt_ok:
        _set_key(physics_lines, "MaxSubstepDeltaTime", str(DEFAULT_MAX_SUBSTEP_DELTA))

    existing_steps = _get_key(physics_lines, "MaxSubsteps")
    try:
        steps_ok = existing_steps is not None and int(float(existing_steps)) >= DEFAULT_MAX_SUBSTEPS
    except ValueError:
        steps_ok = False
    if not steps_ok:
        _set_key(physics_lines, "MaxSubsteps", str(DEFAULT_MAX_SUBSTEPS))

    if _get_key(physics_lines, "MaxPhysicsDeltaTime") is None:
        _set_key(physics_lines, "MaxPhysicsDeltaTime", DEFAULT_MAX_PHYSICS_DELTA)

    rendered: list[str] = []
    for header, lines in sections:
        if header is None:
            rendered.extend(lines)
            continue
        if rendered and rendered[-1].strip() != "":
            rendered.append("")
        rendered.append(header)
        rendered.extend(lines)
    body = "\n".join(rendered).rstrip() + "\n"
    ini_path.write_text(body, encoding="utf-8")
    action = "wrote" if created else "updated"
    return f"{action} deploy physics keys in {ini_path.as_posix()}"


def module_name_from_uproject(uproject_path: Path) -> str:
    stem = re.sub(r"[^A-Za-z0-9_]", "", uproject_path.stem)
    if not stem:
        raise InstallError(f"cannot derive a C++ module name from {uproject_path.name}")
    if stem[0].isdigit():
        stem = f"G{stem}"
    return stem


def _write_if_absent(path: Path, contents: str) -> bool:
    if path.exists():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")
    return True


def scaffold_game_module(project_root: Path, uproject_path: Path) -> str:
    data = json.loads(uproject_path.read_text(encoding="utf-8-sig"))
    modules = data.get("Modules")
    if isinstance(modules, list) and len(modules) > 0:
        return "skipped C++ game module (project already has Modules)"

    name = module_name_from_uproject(uproject_path)
    source_root = project_root / "Source"
    game_target = source_root / f"{name}.Target.cs"
    editor_target = source_root / f"{name}Editor.Target.cs"
    if game_target.exists() or editor_target.exists():
        return f"skipped C++ game module (Source already has {name} targets)"

    wrote: list[str] = []
    if _write_if_absent(
        game_target,
        "using UnrealBuildTool;\n"
        "using System.Collections.Generic;\n\n"
        f"public class {name}Target : TargetRules\n"
        "{\n"
        f"\tpublic {name}Target(TargetInfo Target) : base(Target)\n"
        "\t{\n"
        "\t\tType = TargetType.Game;\n"
        "\t\tDefaultBuildSettings = BuildSettingsVersion.Latest;\n"
        "\t\tIncludeOrderVersion = EngineIncludeOrderVersion.Latest;\n"
        f'\t\tExtraModuleNames.Add("{name}");\n'
        "\t}\n"
        "}\n",
    ):
        wrote.append(game_target.name)
    if _write_if_absent(
        editor_target,
        "using UnrealBuildTool;\n"
        "using System.Collections.Generic;\n\n"
        f"public class {name}EditorTarget : TargetRules\n"
        "{\n"
        f"\tpublic {name}EditorTarget(TargetInfo Target) : base(Target)\n"
        "\t{\n"
        "\t\tType = TargetType.Editor;\n"
        "\t\tDefaultBuildSettings = BuildSettingsVersion.Latest;\n"
        "\t\tIncludeOrderVersion = EngineIncludeOrderVersion.Latest;\n"
        f'\t\tExtraModuleNames.Add("{name}");\n'
        "\t}\n"
        "}\n",
    ):
        wrote.append(editor_target.name)
    if _write_if_absent(
        source_root / name / f"{name}.Build.cs",
        "using UnrealBuildTool;\n\n"
        f"public class {name} : ModuleRules\n"
        "{\n"
        f"\tpublic {name}(ReadOnlyTargetRules Target) : base(Target)\n"
        "\t{\n"
        "\t\tPCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;\n"
        '\t\tPublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });\n'
        "\t}\n"
        "}\n",
    ):
        wrote.append(f"{name}.Build.cs")
    if _write_if_absent(source_root / name / f"{name}.h", "#pragma once\n\n#include \"CoreMinimal.h\"\n"):
        wrote.append(f"{name}.h")
    if _write_if_absent(
        source_root / name / f"{name}.cpp",
        f'#include "{name}.h"\n'
        '#include "Modules/ModuleManager.h"\n\n'
        f'IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, {name}, "{name}");\n',
    ):
        wrote.append(f"{name}.cpp")

    data["Modules"] = [{"Name": name, "Type": "Runtime", "LoadingPhase": "Default"}]
    uproject_path.write_text(json.dumps(data, indent="\t", ensure_ascii=False) + "\n", encoding="utf-8")
    return f"scaffolded empty C++ game module {name} ({', '.join(wrote)})"


def _check(name: str, passed: bool, detail: str) -> PreflightCheck:
    return PreflightCheck(name=name, passed=passed, detail=detail)


def _check_artifact(path: Path) -> PreflightCheck:
    if not path.is_file():
        return _check("artifact", False, f"missing artifact: {path}")
    try:
        from uerl.policy.artifact import PolicyArtifact

        PolicyArtifact.read(path)
    except Exception as exc:  # noqa: BLE001 - report the artifact boundary failure to the operator.
        return _check("artifact", False, f"invalid artifact {path}: {exc}")
    return _check("artifact", True, str(path))


def check_project(*, project_dir: Path, artifact: Path | None = None) -> tuple[PreflightCheck, ...]:
    """Check deployment prerequisites without modifying the target project."""

    project_root, uproject_path = resolve_project(project_dir)
    checks: list[PreflightCheck] = []
    try:
        data = json.loads(uproject_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        return (_check("uproject", False, f"cannot parse {uproject_path}: {exc}"),)

    plugins = {
        str(entry.get("Name")): entry
        for entry in data.get("Plugins", [])
        if isinstance(entry, dict) and entry.get("Name")
    }
    missing_plugins = [name for name in REQUIRED_PLUGINS if plugins.get(name, {}).get("Enabled") is not True]
    checks.append(
        _check(
            "plugins",
            not missing_plugins,
            "all required plugins enabled" if not missing_plugins else f"not enabled: {', '.join(missing_plugins)}",
        )
    )

    plugin_path = project_root / "Plugins" / PLUGIN_NAME / f"{PLUGIN_NAME}.uplugin"
    if not plugin_path.is_file():
        checks.append(_check("plugin", False, f"missing plugin descriptor: {plugin_path}"))
    else:
        try:
            json.loads(plugin_path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError) as exc:
            checks.append(_check("plugin", False, f"cannot parse {plugin_path}: {exc}"))
        else:
            checks.append(_check("plugin", True, str(plugin_path)))

    ini_path = project_root / "Config" / "DefaultEngine.ini"
    physics_lines: list[str] | None = None
    if ini_path.is_file():
        try:
            sections = _parse_sections(ini_path.read_text(encoding="utf-8"))
        except OSError as exc:
            checks.append(_check("physics", False, f"cannot read {ini_path}: {exc}"))
        else:
            for header, lines in sections:
                if header == PHYSICS_SECTION:
                    physics_lines = lines
                    break
    if physics_lines is None:
        checks.append(_check("physics", False, f"missing {PHYSICS_SECTION} in {ini_path}"))
    else:
        failures: list[str] = []
        for key, expected in PHYSICS_BOOLS.items():
            if _get_key(physics_lines, key) != expected:
                failures.append(f"{key}={expected}")
        dt = _get_key(physics_lines, "MaxSubstepDeltaTime")
        try:
            if dt is None or float(dt) > DEFAULT_MAX_SUBSTEP_DELTA:
                failures.append(f"MaxSubstepDeltaTime<={DEFAULT_MAX_SUBSTEP_DELTA}")
        except ValueError:
            failures.append(f"MaxSubstepDeltaTime<={DEFAULT_MAX_SUBSTEP_DELTA}")
        steps = _get_key(physics_lines, "MaxSubsteps")
        try:
            if steps is None or int(float(steps)) < DEFAULT_MAX_SUBSTEPS:
                failures.append(f"MaxSubsteps>={DEFAULT_MAX_SUBSTEPS}")
        except ValueError:
            failures.append(f"MaxSubsteps>={DEFAULT_MAX_SUBSTEPS}")
        checks.append(
            _check(
                "physics",
                not failures,
                "Chaos deployment gate satisfied" if not failures else f"required: {', '.join(failures)}",
            )
        )

    module_name = module_name_from_uproject(uproject_path)
    modules = data.get("Modules")
    has_module_entry = isinstance(modules, list) and any(
        isinstance(entry, dict) and entry.get("Name") == module_name for entry in modules
    )
    target_path = project_root / "Source" / f"{module_name}.Target.cs"
    checks.append(
        _check(
            "game_target",
            has_module_entry and target_path.is_file(),
            str(target_path) if has_module_entry and target_path.is_file() else f"missing Game Target: {target_path}",
        )
    )
    if artifact is not None:
        checks.append(_check_artifact(artifact.resolve()))
    return tuple(checks)


def install(
    *,
    project_dir: Path,
    repo_root: Path,
    force: bool = False,
    from_package: Path | None = None,
    demo: str | None = None,
) -> list[str]:
    if demo not in {None, "phantomx"}:
        raise InstallError(f"unknown demo: {demo}")
    project_root, uproject_path = resolve_project(project_dir)
    reports = [copy_plugin(repo_root=repo_root, project_root=project_root, from_package=from_package, force=force)]
    if demo == "phantomx":
        reports.extend(copy_demo_robot(repo_root=repo_root, project_root=project_root, force=force))
    reports.extend(
        [
            enable_plugins(uproject_path),
            upsert_physics_ini(project_root / "Config" / "DefaultEngine.ini"),
            scaffold_game_module(project_root, uproject_path),
            "demo: PhantomX assets installed" if demo == "phantomx" else "demo: none",
            "next: open the project, compile if a game module was just added, then run UERL.CheckProject",
        ]
    )
    return reports


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


if __name__ == "__main__":
    raise SystemExit(main())

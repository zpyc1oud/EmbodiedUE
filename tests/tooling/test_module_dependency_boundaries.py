"""Gate UE Build.cs, plugin descriptors, and Python package dependency direction."""

from __future__ import annotations

import ast
import json
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ENGINE_PLUGIN_DIR = REPO_ROOT / "engine" / "Plugins" / "UERLEngine"
TRAINING_PLUGIN_DIR = REPO_ROOT / "engine" / "Plugins" / "UERLTraining"
PLUGIN_SOURCE = ENGINE_PLUGIN_DIR / "Source"
TRAINING_SOURCE = TRAINING_PLUGIN_DIR / "Source"
PLUGIN_SOURCES = (PLUGIN_SOURCE, TRAINING_SOURCE)
HOST_UPROJECT = REPO_ROOT / "engine" / "UERLHost.uproject"
PROVIDER_BUILD = PLUGIN_SOURCE / "UERLProvider" / "UERLProvider.Build.cs"
TERRAIN_BUILD = TRAINING_SOURCE / "UERLTerrain" / "UERLTerrain.Build.cs"
POLICY_BUILD = PLUGIN_SOURCE / "UERLPolicy" / "UERLPolicy.Build.cs"
POLICY_PACKAGE = REPO_ROOT / "src" / "uerl" / "policy"

FORBIDDEN_MODULES = frozenset({"UERLWorker", "UERLTransport", "Sockets", "Networking"})
TERRAIN_FORBIDDEN_MODULES = frozenset(
    {"UERLWorker", "UERLProvider", "UERLTransport", "Sockets", "Networking"}
)
MODULE_DEP_RE = re.compile(
    r"(?:Public|Private)DependencyModuleNames\.Add(?:Range)?\((.*?)\)\s*;",
    re.DOTALL,
)
STRING_RE = re.compile(r'"([^"]+)"')
SINGLE_ADD_RE = re.compile(
    r'(?:Public|Private)DependencyModuleNames\.Add\(\s*"([^"]+)"\s*\)'
)


def _module_build_path(
    module: str,
    source_roots: Path | tuple[Path, ...] = PLUGIN_SOURCES,
) -> Path:
    roots = (source_roots,) if isinstance(source_roots, Path) else source_roots
    for root in roots:
        candidate = root / module / f"{module}.Build.cs"
        if candidate.is_file():
            return candidate
    return roots[0] / module / f"{module}.Build.cs"


def _direct_dependencies(build_cs: Path) -> set[str]:
    text = build_cs.read_text(encoding="utf-8")
    deps: set[str] = set()
    for block in MODULE_DEP_RE.findall(text):
        deps.update(STRING_RE.findall(block))
    deps.update(SINGLE_ADD_RE.findall(text))
    return deps


def _dependency_closure(
    root_module: str,
    source_roots: Path | tuple[Path, ...] = PLUGIN_SOURCES,
) -> set[str]:
    seen: set[str] = set()
    stack = [root_module]
    while stack:
        module = stack.pop()
        if module in seen:
            continue
        seen.add(module)
        build_cs = _module_build_path(module, source_roots)
        if not build_cs.is_file():
            continue
        for dep in _direct_dependencies(build_cs):
            if dep not in seen:
                stack.append(dep)
    seen.discard(root_module)
    return seen


def _policy_imports_training(package_root: Path = POLICY_PACKAGE) -> list[str]:
    """Return policy source paths that import ``uerl.training``."""

    leaks: list[str] = []
    for path in sorted(package_root.rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        leaked = False
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                for alias in node.names:
                    if alias.name == "uerl.training" or alias.name.startswith("uerl.training."):
                        leaked = True
            elif isinstance(node, ast.ImportFrom):
                module = node.module or ""
                if module == "uerl.training" or module.startswith("uerl.training."):
                    leaked = True
                if module == "uerl":
                    for alias in node.names:
                        if alias.name == "training" or alias.name.startswith("training."):
                            leaked = True
            if leaked:
                break
        if leaked:
            try:
                leaks.append(str(path.relative_to(REPO_ROOT)))
            except ValueError:
                leaks.append(path.name)
    return sorted(set(leaks))


def test_uerl_provider_build_cs_exists() -> None:
    assert PROVIDER_BUILD.is_file(), f"missing {PROVIDER_BUILD}"


def test_uerl_provider_direct_deps_exclude_forbidden_modules() -> None:
    deps = _direct_dependencies(PROVIDER_BUILD)
    leaked = sorted(deps & FORBIDDEN_MODULES)
    assert not leaked, (
        "UERLProvider.Build.cs must not list "
        f"{sorted(FORBIDDEN_MODULES)}; found {leaked}"
    )


def test_uerl_provider_dependency_closure_excludes_forbidden_modules() -> None:
    closure = _dependency_closure("UERLProvider")
    leaked = sorted(closure & FORBIDDEN_MODULES)
    assert not leaked, (
        "UERLProvider dependency closure must not include "
        f"{sorted(FORBIDDEN_MODULES)}; found {leaked} via {sorted(closure)}"
    )


def test_gate_fails_when_uerl_worker_is_added(tmp_path: Path) -> None:
    """Documented regression check: adding UERLWorker must fail the gate."""
    poisoned = PROVIDER_BUILD.read_text(encoding="utf-8").replace(
        '"UERLInterface",',
        '"UERLInterface",\n\t\t\t"UERLWorker",',
        1,
    )
    assert '"UERLWorker"' in poisoned
    fake = tmp_path / "UERLProvider.Build.cs"
    fake.write_text(poisoned, encoding="utf-8")
    leaked = sorted(_direct_dependencies(fake) & FORBIDDEN_MODULES)
    assert leaked == ["UERLWorker"]


def test_uerl_terrain_build_cs_exists() -> None:
    assert TERRAIN_BUILD.is_file(), f"missing {TERRAIN_BUILD}"


def test_uerl_terrain_direct_deps_exclude_forbidden_modules() -> None:
    deps = _direct_dependencies(TERRAIN_BUILD)
    leaked = sorted(deps & TERRAIN_FORBIDDEN_MODULES)
    assert not leaked, (
        "UERLTerrain.Build.cs must not list "
        f"{sorted(TERRAIN_FORBIDDEN_MODULES)}; found {leaked}"
    )


def test_uerl_terrain_dependency_closure_excludes_forbidden_modules() -> None:
    closure = _dependency_closure("UERLTerrain")
    leaked = sorted(closure & TERRAIN_FORBIDDEN_MODULES)
    assert not leaked, (
        "UERLTerrain dependency closure must not include "
        f"{sorted(TERRAIN_FORBIDDEN_MODULES)}; found {leaked} via {sorted(closure)}"
    )


def test_terrain_gate_fails_when_uerl_worker_is_added(tmp_path: Path) -> None:
    """Documented regression check: adding UERLWorker must fail the terrain gate."""
    poisoned = TERRAIN_BUILD.read_text(encoding="utf-8").replace(
        '"UERLInterface",',
        '"UERLInterface",\n\t\t\t"UERLWorker",',
        1,
    )
    assert '"UERLWorker"' in poisoned
    fake = tmp_path / "UERLTerrain.Build.cs"
    fake.write_text(poisoned, encoding="utf-8")
    leaked = sorted(_direct_dependencies(fake) & TERRAIN_FORBIDDEN_MODULES)
    assert leaked == ["UERLWorker"]


def test_uerl_policy_build_cs_exists() -> None:
    assert POLICY_BUILD.is_file(), f"missing {POLICY_BUILD}"


def test_uerl_policy_direct_deps_exclude_forbidden_modules() -> None:
    deps = _direct_dependencies(POLICY_BUILD)
    leaked = sorted(deps & FORBIDDEN_MODULES)
    assert not leaked, (
        "UERLPolicy.Build.cs must not list "
        f"{sorted(FORBIDDEN_MODULES)}; found {leaked}"
    )


def test_uerl_policy_dependency_closure_excludes_forbidden_modules() -> None:
    closure = _dependency_closure("UERLPolicy")
    leaked = sorted(closure & FORBIDDEN_MODULES)
    assert not leaked, (
        "UERLPolicy dependency closure must not include "
        f"{sorted(FORBIDDEN_MODULES)}; found {leaked} via {sorted(closure)}"
    )


def test_policy_gate_fails_when_uerl_worker_is_added(tmp_path: Path) -> None:
    """Documented regression check: adding UERLWorker must fail the policy gate."""
    poisoned = POLICY_BUILD.read_text(encoding="utf-8").replace(
        '"UERLInterface",',
        '"UERLInterface",\n\t\t\t"UERLWorker",',
        1,
    )
    assert '"UERLWorker"' in poisoned
    fake = tmp_path / "UERLPolicy.Build.cs"
    fake.write_text(poisoned, encoding="utf-8")
    leaked = sorted(_direct_dependencies(fake) & FORBIDDEN_MODULES)
    assert leaked == ["UERLWorker"]


def test_plugin_descriptors_isolate_game_runtime_from_training_modules() -> None:
    engine_descriptor = json.loads((ENGINE_PLUGIN_DIR / "UERLEngine.uplugin").read_text(encoding="utf-8"))
    training_descriptor = json.loads((TRAINING_PLUGIN_DIR / "UERLTraining.uplugin").read_text(encoding="utf-8"))
    host_uproject = json.loads(HOST_UPROJECT.read_text(encoding="utf-8"))

    engine_modules = {entry["Name"] for entry in engine_descriptor["Modules"]}
    engine_plugins = {entry["Name"]: entry["Enabled"] for entry in engine_descriptor["Plugins"]}
    assert engine_modules == {
        "UERLInterface",
        "UERLPolicy",
        "UERLProvider",
        "UERLRobot",
        "UERLPolicyEditor",
    }
    assert engine_modules.isdisjoint({"UERLWorker", "UERLTransport", "UERLTerrain"})
    assert engine_plugins == {"NNERuntimeORT": True}
    assert "ProceduralMeshComponent" not in engine_plugins

    training_modules = {entry["Name"] for entry in training_descriptor["Modules"]}
    training_plugins = {entry["Name"]: entry["Enabled"] for entry in training_descriptor["Plugins"]}
    assert training_modules == {"UERLTerrain", "UERLTransport", "UERLWorker"}
    assert training_plugins == {"UERLEngine": True, "ProceduralMeshComponent": True}

    host_plugins = {entry["Name"]: entry["Enabled"] for entry in host_uproject["Plugins"]}
    assert host_plugins == {
        "UERLEngine": True,
        "UERLTraining": True,
        "ProceduralMeshComponent": True,
        "NNERuntimeORT": True,
    }


def test_policy_package_exists() -> None:
    assert POLICY_PACKAGE.is_dir(), f"missing {POLICY_PACKAGE}"
    assert (POLICY_PACKAGE / "artifact.py").is_file()


def test_policy_must_not_import_training() -> None:
    """Ticket 13 gate: ``uerl.policy`` must not depend on ``uerl.training``."""

    leaks = _policy_imports_training()
    assert not leaks, f"policy must not import uerl.training; found in {leaks}"


def test_policy_training_import_gate_fails_when_poisoned(tmp_path: Path) -> None:
    """Documented regression check: importing training from policy must fail the gate."""

    poisoned = tmp_path / "policy"
    poisoned.mkdir()
    (poisoned / "artifact.py").write_text(
        "from uerl.training import run_training\n",
        encoding="utf-8",
    )
    leaks = _policy_imports_training(poisoned)
    assert leaks == ["artifact.py"]

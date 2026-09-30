"""Install a UE project with the UERL plugin, optional demo robot, physics, and empty game module."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
from types import ModuleType

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "install_into_project.py"


def _load() -> ModuleType:
    spec = importlib.util.spec_from_file_location("install_into_project", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _layout(root: Path) -> tuple[Path, Path]:
    repo = root / "repo"
    plugin = repo / "engine" / "Plugins" / "UERLEngine"
    _write(plugin / "UERLEngine.uplugin", '{"FileVersion": 3, "VersionName": "1.0.0"}\n')
    _write(plugin / "Binaries" / "Win64" / "skip.dll", "bin")
    robot = repo / "engine" / "Content" / "Robots" / "PhantomX"
    for name in ("SK_PhantomX.uasset", "SK_PhantomX_Skeleton.uasset", "PA_PhantomX.uasset"):
        _write(robot / name, name)
    game = root / "game"
    _write(game / "CleanGame.uproject", json.dumps({"FileVersion": 3, "EngineAssociation": "5.8"}) + "\n")
    return repo, game


def test_empty_blueprint_project_gets_plugin_robot_physics_and_game_module(tmp_path: Path) -> None:
    install = _load()
    repo, game = _layout(tmp_path)

    reports = install.install(project_dir=game, repo_root=repo, demo="phantomx")

    data = json.loads((game / "CleanGame.uproject").read_text(encoding="utf-8"))
    enabled = {entry["Name"]: entry["Enabled"] for entry in data["Plugins"]}
    assert enabled["UERLEngine"] is True
    assert enabled["ProceduralMeshComponent"] is True
    assert enabled["NNERuntimeORT"] is True
    assert data["Modules"][0]["Name"] == "CleanGame"
    assert (game / "Plugins" / "UERLEngine" / "UERLEngine.uplugin").is_file()
    assert not (game / "Plugins" / "UERLEngine" / "Binaries").exists()
    for name in ("SK_PhantomX.uasset", "SK_PhantomX_Skeleton.uasset", "PA_PhantomX.uasset"):
        assert (game / "Content" / "Robots" / "PhantomX" / name).is_file()
    ini = (game / "Config" / "DefaultEngine.ini").read_text(encoding="utf-8")
    assert "bTickPhysicsAsync=False" in ini
    assert "bSubstepping=True" in ini
    assert "bSubsteppingAsync=False" in ini
    assert "MaxSubstepDeltaTime=0.005" in ini
    assert "MaxSubsteps=7" in ini
    assert "MaxPhysicsDeltaTime=0.033333" in ini
    assert (game / "Source" / "CleanGame.Target.cs").is_file()
    assert (game / "Source" / "CleanGameEditor.Target.cs").is_file()
    assert (game / "Source" / "CleanGame" / "CleanGame.cpp").is_file()
    assert any("scaffolded empty C++ game module CleanGame" in line for line in reports)


def test_existing_assets_and_stricter_physics_are_preserved_unless_forced(tmp_path: Path) -> None:
    install = _load()
    repo, game = _layout(tmp_path)
    install.install(project_dir=game, repo_root=repo, demo="phantomx")

    marker = game / "Plugins" / "UERLEngine" / "keep.txt"
    marker.write_text("keep", encoding="utf-8")
    mesh = game / "Content" / "Robots" / "PhantomX" / "SK_PhantomX.uasset"
    mesh.write_text("original-mesh", encoding="utf-8")
    (game / "Config" / "DefaultEngine.ini").write_text(
        "[/Script/Engine.PhysicsSettings]\n"
        "bTickPhysicsAsync=True\n"
        "bSubstepping=False\n"
        "bSubsteppingAsync=True\n"
        "MaxSubstepDeltaTime=0.002\n"
        "MaxSubsteps=9\n"
        "MaxPhysicsDeltaTime=0.016667\n"
        "SomeOtherKey=Stay\n",
        encoding="utf-8",
    )
    data = json.loads((game / "CleanGame.uproject").read_text(encoding="utf-8"))
    data["Modules"] = [{"Name": "CleanGame", "Type": "Runtime", "LoadingPhase": "Default"}]
    (game / "CleanGame.uproject").write_text(json.dumps(data, indent="\t") + "\n", encoding="utf-8")
    (game / "Source" / "CleanGame.Target.cs").write_text("existing-target", encoding="utf-8")

    second = install.install(project_dir=game, repo_root=repo, demo="phantomx")
    assert marker.read_text(encoding="utf-8") == "keep"
    assert mesh.read_text(encoding="utf-8") == "original-mesh"
    assert any("skipped plugin" in line for line in second)
    assert any("skipped SK_PhantomX.uasset" in line for line in second)
    assert any("skipped C++ game module" in line for line in second)
    ini = (game / "Config" / "DefaultEngine.ini").read_text(encoding="utf-8")
    assert "bTickPhysicsAsync=False" in ini
    assert "bSubstepping=True" in ini
    assert "MaxSubstepDeltaTime=0.002" in ini
    assert "MaxSubsteps=9" in ini
    assert "MaxPhysicsDeltaTime=0.016667" in ini
    assert "SomeOtherKey=Stay" in ini
    assert (game / "Source" / "CleanGame.Target.cs").read_text(encoding="utf-8") == "existing-target"

    plugin_descriptor = repo / "engine" / "Plugins" / "UERLEngine" / "UERLEngine.uplugin"
    plugin_descriptor.write_text('{"forced": true}\n', encoding="utf-8")
    mesh_source = repo / "engine" / "Content" / "Robots" / "PhantomX" / "SK_PhantomX.uasset"
    mesh_source.write_text("new-mesh", encoding="utf-8")
    install.install(project_dir=game, repo_root=repo, force=True, demo="phantomx")
    assert not marker.exists()
    assert (game / "Plugins" / "UERLEngine" / "UERLEngine.uplugin").read_text(encoding="utf-8") == '{"forced": true}\n'
    assert mesh.read_text(encoding="utf-8") == "new-mesh"


def test_from_package_copies_the_given_directory(tmp_path: Path) -> None:
    install = _load()
    repo, game = _layout(tmp_path)
    package = tmp_path / "package"
    _write(package / "UERLEngine.uplugin", '{"from": "package"}\n')
    _write(package / "Binaries" / "Win64" / "UnrealEditor-UERLInterface.dll", "dll")

    install.install(project_dir=game, repo_root=repo, from_package=package)

    copied = game / "Plugins" / "UERLEngine" / "UERLEngine.uplugin"
    assert copied.read_text(encoding="utf-8") == '{"from": "package"}\n'
    assert (game / "Plugins" / "UERLEngine" / "Binaries" / "Win64" / "UnrealEditor-UERLInterface.dll").is_file()


def test_default_install_is_runtime_only(tmp_path: Path) -> None:
    install = _load()
    repo, game = _layout(tmp_path)

    reports = install.install(project_dir=game, repo_root=repo)

    assert not (game / "Content" / "Robots" / "PhantomX").exists()
    assert "demo: none" in reports


def test_unknown_demo_is_rejected(tmp_path: Path) -> None:
    install = _load()
    repo, game = _layout(tmp_path)

    try:
        install.install(project_dir=game, repo_root=repo, demo="cartpole")
    except install.InstallError as exc:
        assert "unknown demo" in str(exc)
    else:
        raise AssertionError("unknown demo should be rejected")


def test_preflight_is_read_only_and_passes_after_install(tmp_path: Path) -> None:
    install = _load()
    repo, game = _layout(tmp_path)

    before = install.check_project(project_dir=game)
    assert not all(check.passed for check in before)

    install.install(project_dir=game, repo_root=repo)
    after = install.check_project(project_dir=game)

    assert all(check.passed for check in after)
    assert not (game / "Content" / "Robots" / "PhantomX").exists()


def test_preflight_reports_physics_and_artifact_failures(tmp_path: Path) -> None:
    install = _load()
    repo, game = _layout(tmp_path)
    install.install(project_dir=game, repo_root=repo)

    ini_path = game / "Config" / "DefaultEngine.ini"
    ini_path.write_text(
        ini_path.read_text(encoding="utf-8").replace("MaxSubsteps=7", "MaxSubsteps=1"),
        encoding="utf-8",
    )
    artifact = tmp_path / "invalid.uerlpol2"
    artifact.write_bytes(b"not an artifact")

    checks = install.check_project(project_dir=game, artifact=artifact)

    failed = {check.name for check in checks if not check.passed}
    assert {"physics", "artifact"} <= failed

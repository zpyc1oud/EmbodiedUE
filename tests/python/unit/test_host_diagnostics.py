"""Static checks use filesystem fixtures, never a UE process."""

import json
from pathlib import Path

import pytest

from uerl.host import HostProfile, check_host, resolve_host_profile


def test_missing_host_paths_have_corrective_actions(tmp_path: Path) -> None:
    profile = tmp_path / "host.toml"
    profile.write_text("", encoding="utf-8")
    selected = resolve_host_profile(
        profile_path=profile,
        ue_executable=tmp_path / "missing.exe",
        project=tmp_path / "missing.uproject",
    )
    report = check_host(selected)
    failures = {item.code: item for item in report.checks if not item.ok}
    assert "UE_EXECUTABLE_MISSING" in failures
    assert "PROJECT_MISSING" in failures
    assert all(item.action for item in failures.values())
    assert not report.ok


def test_complete_fixture_passes_static_checks_only(tmp_path: Path) -> None:
    selected = make_host(tmp_path)
    before = {path: path.read_bytes() for path in tmp_path.rglob("*") if path.is_file()}
    report = check_host(selected)
    assert report.ok
    assert {path: path.read_bytes() for path in tmp_path.rglob("*") if path.is_file()} == before


@pytest.mark.parametrize("contents", [None, b"", b"version https://git-lfs.github.com/spec/v1\noid sha256:123\n"])
def test_missing_empty_and_lfs_assets_fail_with_download_action(tmp_path: Path, contents: bytes | None) -> None:
    selected = make_host(tmp_path)
    asset = selected.project.parent / "Content/Robots/CartPole/SKM_CartPole.uasset"
    if contents is None:
        asset.unlink()
    else:
        asset.write_bytes(contents)
    report = check_host(selected)
    failures = [check for check in report.checks if not check.ok]
    assert len(failures) == 1
    assert failures[0].path == str(asset)
    assert failures[0].code == "CARTPOLE_CONTENT_MISSING"
    assert "git lfs pull" in failures[0].action


@pytest.mark.parametrize(
    "payload", ["{broken", "[]", '{"Plugins": [5]}', '{"Plugins": [{"Name": "UERLEngine", "Enabled": "yes"}]}']
)
def test_malformed_project_is_actionable(tmp_path: Path, payload: str) -> None:
    selected = make_host(tmp_path)
    selected.project.write_text(payload, encoding="utf-8")
    assert any(not check.ok and check.action for check in check_host(selected).checks)


def test_disabled_dependency_and_missing_build_are_reported_together(tmp_path: Path) -> None:
    selected = make_host(tmp_path)
    project = json.loads(selected.project.read_text(encoding="utf-8"))
    project["Plugins"][2]["Enabled"] = False
    selected.project.write_text(json.dumps(project), encoding="utf-8")
    (selected.project.parent / "Binaries/Win64/UnrealEditor-UERLHost.dll").unlink()
    failures = [check for check in check_host(selected).checks if not check.ok]
    assert {check.code for check in failures} == {"PROJECT_PLUGIN_ENABLED", "HOST_BUILD_MISSING"}
    assert any("NNERuntimeORT" in check.action for check in failures)
    assert any("Build UERLHostEditor" in check.action for check in failures)


@pytest.mark.parametrize("change", ["missing", "invalid"])
def test_engine_dependency_descriptor_is_checked(tmp_path: Path, change: str) -> None:
    selected = make_host(tmp_path)
    plugin = tmp_path / "UE/Engine/Plugins/Experimental/NNERuntimeORT/NNERuntimeORT.uplugin"
    if change == "missing":
        plugin.unlink()
    else:
        plugin.write_text("not json", encoding="utf-8")
    failures = [check for check in check_host(selected).checks if not check.ok]
    assert len(failures) == 1
    assert failures[0].code == ("ENGINE_PLUGIN_MISSING" if change == "missing" else "ENGINE_PLUGIN_INVALID")
    assert "NNERuntimeORT" in failures[0].action


def test_directory_instead_of_executable_fails(tmp_path: Path) -> None:
    selected = make_host(tmp_path)
    selected.ue_executable.unlink()
    selected.ue_executable.mkdir()
    assert any(check.code == "UE_EXECUTABLE_MISSING" for check in check_host(selected).checks)


def make_host(root: Path) -> HostProfile:
    """Create dummy files, not usable UE binaries or valid UE content."""
    files = [
        "UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe",
        "UE/Engine/Content/Maps/Entry.umap",
        "host project/Binaries/Win64/UnrealEditor-UERLHost.dll",
        "host project/Plugins/UERLEngine/Binaries/Win64/UnrealEditor-UERLWorker.dll",
        *[
            f"host project/Content/Robots/CartPole/{name}.uasset"
            for name in ("SK_CartPole", "SKM_CartPole", "PA_CartPole", "CartPole")
        ],
    ]
    for relative in files:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"fixture content, not an actual UE file")
    for relative in (
        "host project/Plugins/UERLEngine/UERLEngine.uplugin",
        "UE/Engine/Plugins/Runtime/ProceduralMeshComponent/ProceduralMeshComponent.uplugin",
        "UE/Engine/Plugins/Experimental/NNERuntimeORT/NNERuntimeORT.uplugin",
    ):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('{"FileVersion": 3}', encoding="utf-8")
    project = root / "host project/UERLHost.uproject"
    project.write_text(
        json.dumps(
            {
                "Plugins": [
                    {"Name": name, "Enabled": True}
                    for name in (
                        "UERLEngine",
                        "ProceduralMeshComponent",
                        "NNERuntimeORT",
                    )
                ]
            }
        ),
        encoding="utf-8",
    )
    profile = root / "host.toml"
    profile.write_text(
        "project = 'host project/UERLHost.uproject'\nue_executable = 'UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe'\n",
        encoding="utf-8",
    )
    return resolve_host_profile(profile_path=profile)

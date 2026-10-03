"""Host selection is reusable without a Session or UE installation."""

from pathlib import Path

import pytest

from uerl.errors import ConfigError
from uerl.host import resolve_host_profile


def test_profile_paths_are_relative_to_profile_and_explicit_values_win(tmp_path: Path) -> None:
    profile = tmp_path / "host.toml"
    profile.write_text(
        "ue_executable = 'UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe'\nproject = 'host/UERLHost.uproject'\n",
        encoding="utf-8",
    )
    selected = resolve_host_profile(profile_path=profile, project=Path("explicit.uproject"))
    assert selected.ue_executable == tmp_path / "UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    assert selected.project == Path("explicit.uproject").resolve()
    assert selected.sources == {"ue_executable": "profile", "project": "explicit"}


@pytest.mark.parametrize("selector", ["explicit", "environment", "default"])
def test_profile_selection_and_field_fallback(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    selector: str,
) -> None:
    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.delenv("UERL_HOST_PROFILE", raising=False)
    implicit = tmp_path / ".uerl/host.toml"
    implicit.parent.mkdir()
    implicit.write_text("project = 'default.uproject'", encoding="utf-8")
    environment = tmp_path / "environment.toml"
    environment.write_text("project = 'environment.uproject'", encoding="utf-8")
    explicit = tmp_path / "explicit.toml"
    explicit.write_text("project = 'explicit.uproject'", encoding="utf-8")
    if selector != "default":
        monkeypatch.setenv("UERL_HOST_PROFILE", str(environment))
    selected = resolve_host_profile(profile_path=explicit if selector == "explicit" else None)
    expected = implicit.parent / "default.uproject" if selector == "default" else tmp_path / f"{selector}.uproject"
    assert selected.project == expected
    assert selected.sources == {"ue_executable": "default", "project": "profile"}


@pytest.mark.parametrize(
    "content", ["broken = [", "project = 2", "project = ''", "typo = 'x'", "[host]\nproject = 'x'"]
)
def test_invalid_profiles_fail_even_with_explicit_overrides(tmp_path: Path, content: str) -> None:
    profile = tmp_path / "host.toml"
    profile.write_text(content, encoding="utf-8")
    with pytest.raises(ConfigError) as error:
        resolve_host_profile(profile_path=profile, project=tmp_path / "override.uproject")
    assert error.value.code == "INVALID_HOST_PROFILE"


def test_only_missing_implicit_default_is_optional(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.delenv("UERL_HOST_PROFILE", raising=False)
    assert resolve_host_profile().sources == {"ue_executable": "default", "project": "default"}
    missing = tmp_path / "missing.toml"
    with pytest.raises(ConfigError, match="Cannot read host profile"):
        resolve_host_profile(profile_path=missing)
    monkeypatch.setenv("UERL_HOST_PROFILE", str(missing))
    with pytest.raises(ConfigError, match="Cannot read host profile"):
        resolve_host_profile()


def test_empty_environment_selector_uses_optional_default(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.setenv("UERL_HOST_PROFILE", "")
    assert resolve_host_profile().sources == {"ue_executable": "default", "project": "default"}

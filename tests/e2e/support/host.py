"""Share product host selection and per-invocation paths with test processes."""

from __future__ import annotations

import os
import tempfile
from dataclasses import replace
from pathlib import Path

from uerl.host import HostProfile, resolve_host_profile


def resolve_test_host(
    *,
    profile_path: Path | None = None,
    ue_executable: Path | None = None,
    project: Path | None = None,
) -> HostProfile:
    """Use product precedence, with engine-root variables as a default fallback."""
    executable_env = os.environ.get("UERL_TEST_UE_EXECUTABLE")
    project_env = os.environ.get("UERL_TEST_PROJECT")
    profile = resolve_host_profile(
        profile_path=profile_path,
        ue_executable=ue_executable if ue_executable is not None else Path(executable_env) if executable_env else None,
        project=project if project is not None else Path(project_env) if project_env else None,
    )
    for variable in ("UE_ROOT", "UE_58_ROOT"):
        root = os.environ.get(variable)
        if root and profile.sources["ue_executable"] == "default":
            return replace(
                profile,
                ue_executable=(Path(root).expanduser() / "Engine/Binaries/Win64/UnrealEditor-Cmd.exe").resolve(),
                sources={**profile.sources, "ue_executable": variable},
            )
    return profile


def host_environment(profile: HostProfile, output_directory: Path) -> dict[str, str]:
    """Pass the selected paths to pytest and its Worker helpers."""
    return {
        **os.environ,
        **({"UERL_HOST_PROFILE": str(profile.profile_path)} if profile.profile_path.is_file() else {}),
        "UERL_TEST_UE_EXECUTABLE": str(profile.ue_executable),
        "UERL_TEST_PROJECT": str(profile.project),
        "UERL_TEST_OUTPUT_DIR": str(output_directory),
    }


def worker_log_path(name: str) -> str:
    """Allocate a unique log without replacing a previous test's evidence."""
    directory = Path(os.environ.get("UERL_TEST_OUTPUT_DIR", tempfile.gettempdir()))
    directory.mkdir(parents=True, exist_ok=True)
    descriptor, path = tempfile.mkstemp(prefix=name + "-", suffix=".log", dir=directory)
    os.close(descriptor)
    return path

"""Read-only filesystem checks for the bundled Windows/UE CartPole host."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from .profile import HostProfile


@dataclass(frozen=True, slots=True)
class HostCheck:
    """One static observation with a stable code and corrective action."""

    code: str
    ok: bool
    path: str
    message: str
    action: str = ""


@dataclass(frozen=True, slots=True)
class HostReport:
    """Static evidence only; success does not establish runtime readiness."""

    checks: tuple[HostCheck, ...]

    @property
    def ok(self) -> bool:
        return all(check.ok for check in self.checks)


def check_host(profile: HostProfile) -> HostReport:
    """Inspect the selected host without launching processes or changing files.

    This bounded preflight checks the bundled CartPole content and required plugin
    descriptors. UE still owns asset loading, transitive dependencies and build ABI.
    """

    checks: list[HostCheck] = []

    def file_check(path: Path, code: str, action: str) -> bool:
        try:
            with path.open("rb") as stream:
                prefix = stream.read(64)
            if not prefix:
                message = "File is empty."
            elif prefix.startswith(b"version https://git-lfs.github.com/spec/v1"):
                message = "Git LFS pointer; asset data has not been downloaded."
            else:
                checks.append(HostCheck(code, True, str(path), "File is present and is not an LFS pointer."))
                return True
        except OSError as exc:
            message = f"File is missing or unreadable: {exc}"
        checks.append(HostCheck(f"{code}_MISSING", False, str(path), message, action))
        return False

    def descriptor(path: Path, code: str, action: str) -> dict[str, object] | None:
        if not file_check(path, code, action):
            return None
        try:
            value = json.loads(path.read_text(encoding="utf-8-sig"))
            if not isinstance(value, dict):
                raise ValueError("expected a JSON object")
        except (OSError, ValueError) as exc:
            checks.append(HostCheck(f"{code}_INVALID", False, str(path), str(exc), action))
            return None
        return value

    file_check(profile.ue_executable, "UE_EXECUTABLE", "Install UE 5.8 or set --ue-executable to UnrealEditor-Cmd.exe.")
    project = descriptor(profile.project, "PROJECT", "Set --project to a readable UERLHost.uproject JSON file.")
    if profile.project.suffix.lower() != ".uproject":
        checks.append(
            HostCheck(
                "PROJECT_SUFFIX",
                False,
                str(profile.project),
                "Expected a .uproject file, not a directory.",
                "Set --project to the .uproject file.",
            )
        )
    if project is not None:
        plugins = project.get("Plugins", [])
        if not isinstance(plugins, list) or any(
            not isinstance(item, dict)
            or not isinstance(item.get("Name"), str)
            or ("Enabled" in item and not isinstance(item["Enabled"], bool))
            for item in plugins
        ):
            checks.append(
                HostCheck(
                    "PROJECT_PLUGINS_INVALID",
                    False,
                    str(profile.project),
                    "Invalid Plugins array.",
                    "Restore the Plugins array from engine/UERLHost.uproject.",
                )
            )
        else:
            enabled = {item["Name"]: item.get("Enabled") for item in plugins}
            for name in ("UERLEngine", "ProceduralMeshComponent", "NNERuntimeORT"):
                # The bundled host explicitly enables all three required plugins.
                ok = enabled.get(name) is True
                checks.append(
                    HostCheck(
                        "PROJECT_PLUGIN_ENABLED",
                        ok,
                        str(profile.project),
                        f"{name}: explicitly enabled={ok}.",
                        "" if ok else f"Enable {name} in the host project, as in engine/UERLHost.uproject.",
                    )
                )

    plugin_root = profile.project.parent / "Plugins/UERLEngine"
    descriptor(plugin_root / "UERLEngine.uplugin", "UERL_PLUGIN", "Install the bundled UERLEngine plugin in the host.")
    build_action = "Build UERLHostEditor Win64 Development with the selected UE 5.8 installation; see README.md."
    file_check(profile.project.parent / "Binaries/Win64/UnrealEditor-UERLHost.dll", "HOST_BUILD", build_action)
    file_check(plugin_root / "Binaries/Win64/UnrealEditor-UERLWorker.dll", "WORKER_BUILD", build_action)
    content_action = "Run git lfs pull in the host checkout and restore the bundled CartPole content."
    for name in ("SK_CartPole", "SKM_CartPole", "PA_CartPole", "CartPole"):
        file_check(
            profile.project.parent / f"Content/Robots/CartPole/{name}.uasset", "CARTPOLE_CONTENT", content_action
        )

    executable = profile.ue_executable
    if (
        executable.name.lower() != "unrealeditor-cmd.exe"
        or executable.parent.name.lower() != "win64"
        or executable.parent.parent.name.lower() != "binaries"
        or executable.parent.parent.parent.name.lower() != "engine"
    ):
        checks.append(
            HostCheck(
                "UE_LAYOUT",
                False,
                str(executable),
                "Expected Engine/Binaries/Win64/UnrealEditor-Cmd.exe.",
                "Select UnrealEditor-Cmd.exe from the supported Windows UE 5.8 installation.",
            )
        )
    else:
        engine = executable.parent.parent.parent
        file_check(
            engine / "Content/Maps/Entry.umap", "ENTRY_MAP", "Repair the selected UE installation's Engine content."
        )
        for name in ("ProceduralMeshComponent", "NNERuntimeORT"):
            try:
                candidates = sorted((engine / "Plugins").rglob(f"{name}.uplugin"))
            except OSError as exc:
                checks.append(
                    HostCheck(
                        "ENGINE_PLUGIN_UNREADABLE",
                        False,
                        str(engine / "Plugins"),
                        str(exc),
                        "Restore read access to the selected UE installation's Plugins directory.",
                    )
                )
                continue
            action = f"Install/enable the {name} engine plugin in the selected UE 5.8 installation."
            if not candidates:
                checks.append(
                    HostCheck("ENGINE_PLUGIN_MISSING", False, str(engine / "Plugins"), f"{name} not found.", action)
                )
            else:
                descriptor(candidates[0], "ENGINE_PLUGIN", action)
    return HostReport(tuple(checks))

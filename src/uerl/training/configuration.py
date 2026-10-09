"""Build launch and Task configuration without importing the learning runtime."""

from __future__ import annotations

import socket
from collections.abc import Mapping
from pathlib import Path

from ..application.run_config import resolve_run_config
from ..core.config import PresentationMode, ResolvedRunConfig
from ..core.config.snapshot import encode_worker_args

DEFAULT_TRAINING_MAP = "/Game/Maps/NewMap"
# Game DefaultEngine.ini uses deploy substepping. A Worker process must lock
# the Chaos scene to one integration step per engine frame before the scene is created.
WORKER_LOCKSTEP_PHYSICS_ARGS: tuple[str, ...] = (
    "-ini:Engine:[/Script/Engine.PhysicsSettings]:bTickPhysicsAsync=False",
    "-ini:Engine:[/Script/Engine.PhysicsSettings]:bSubstepping=False",
    "-ini:Engine:[/Script/Engine.PhysicsSettings]:bSubsteppingAsync=False",
)


def build_launch_overrides(
    *,
    ue_executable: Path,
    project: Path,
    map_name: str,
    port: int | None = None,
    presentation: PresentationMode = PresentationMode.NONE,
    window_size: tuple[int, int] = (640, 360),
) -> dict[str, str]:
    """Build generic Session launch values for one UE Worker Run.

    Args:
        ue_executable: Path to ``UnrealEditor-Cmd.exe``.
        project: UE project containing the UERL plugin.
        map_name: UE map argument passed to the Worker.
        port: Bridge port; a free local port is reserved when omitted.
        presentation: Worker presentation mode. ``NONE`` launches a headless
            Null-RHI Worker; ``VIEWPORT`` installs the UERL observer; ``GAMEPLAY``
            preserves the map's PlayerController and Pawn in a visible window.
        window_size: ``(width, height)`` used only in viewport mode.

    Returns:
        A mapping of Session config paths to their raw override values. The UE
        command line derives entirely from ``session.worker_args``; the UE side
        validates presentation from ``-uerlpresentation=`` and rejects
        ``-nullrhi`` in viewport mode, so the two modes emit disjoint RHI flags.
    """

    if map_name == "/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo":
        optional_map = project.parent / "Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap"
        present = optional_map.is_file() and optional_map.stat().st_size > 0
        if present:
            with optional_map.open("rb") as stream:
                present = not stream.read(64).startswith(b"version https://git-lfs.github.com/spec/v1")
        if not present:
            raise FileNotFoundError(
                f"Optional Stylized Egypt map is missing or only an LFS pointer: {optional_map}. "
                "Acquire/install your own copy from "
                "https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd "
                "and follow docs/how-to/optional-egypt-demo.md. No replacement map was selected."
            )

    bridge_port = port if port is not None else _free_port()
    worker_args = [
        str(project),
        map_name,
        "-game",
        f"-uerlport={bridge_port}",
        f"-uerlpresentation={presentation.value}",
        *_presentation_args(presentation, window_size),
        "-nopause",
        "-nosplash",
        "-stdout",
        "-FullStdOutLogOutput",
        *WORKER_LOCKSTEP_PHYSICS_ARGS,
    ]
    return {
        "session.worker_executable": str(ue_executable),
        "session.worker_args": encode_worker_args(worker_args),
        "session.map_path": map_name,
        "session.port": str(bridge_port),
        "session.presentation_mode": presentation.value,
    }


def _presentation_args(
    presentation: PresentationMode,
    window_size: tuple[int, int],
) -> list[str]:
    """Return the RHI and windowing flags required by one presentation mode."""

    if presentation is PresentationMode.NONE:
        return ["-nullrhi", "-unattended", "-nosound"]
    width, height = window_size
    if presentation is PresentationMode.GAMEPLAY:
        return [
            "-windowed",
            f"-ResX={width}",
            f"-ResY={height}",
            "-nosound",
        ]
    return [
        "-windowed",
        f"-ResX={width}",
        f"-ResY={height}",
        "-nosound",
        # Viewport training follows the first generic robot instead of using a fixed shot.
        "-uerlfollowrobot=1",
    ]


def build_run_config(
    task_id: str,
    *,
    overrides: Mapping[str, str] | None = None,
) -> ResolvedRunConfig:
    """Resolve one registered Task through the shared Config boundary."""

    return resolve_run_config(task_id, None, overrides or {}).config


def _free_port() -> int:
    """Reserve one local TCP port for a launched UE Worker."""

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


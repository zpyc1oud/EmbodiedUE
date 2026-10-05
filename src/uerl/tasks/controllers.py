"""Play-time command publishers. Training and export do not use this registry."""

from __future__ import annotations

import math
import sys
from collections.abc import Callable, Mapping, Sequence, Set

import torch

from uerl.core.direct.task import CommandSource, DirectTask
from uerl.errors import ConfigError

ControllerFactory = Callable[[DirectTask, int], CommandSource]
_FACTORIES: dict[str, ControllerFactory] = {}

_KEY_W = 0x57
_KEY_A = 0x41
_KEY_S = 0x53
_KEY_D = 0x44
_KEY_Q = 0x51
_KEY_E = 0x45


def register_play_controller(name: str, factory: ControllerFactory) -> None:
    """Register a named play controller beside the built-in publishers."""

    if not name or name in {"task", "player", "fixed"}:
        raise ConfigError(
            f"play controller name {name!r} is reserved",
            code="CONFIG_OUT_OF_RANGE",
            path="controller",
        )
    if name in _FACTORIES:
        raise ConfigError(
            f"play controller name {name!r} is already registered",
            code="CONFIG_OUT_OF_RANGE",
            path="controller",
        )
    _FACTORIES[name] = factory


def known_play_controllers() -> tuple[str, ...]:
    return ("task", "player", "fixed", *tuple(_FACTORIES))


def create_play_controller(
    name: str,
    task: DirectTask,
    batch_size: int,
    *,
    fixed_velocity: Sequence[float] | None = None,
) -> CommandSource | None:
    """Return a replacement command source, or None to keep the task's own."""

    if name == "task":
        return None
    if name == "player":
        channels = task.command_channels()
        if channels.get("velocity") != 3:
            raise ConfigError(
                "player control requires a velocity command channel of width 3",
                code="CONFIG_OUT_OF_RANGE",
                path="controller",
            )
        config = getattr(task, "phantomx_config", None)
        if config is None:
            raise ConfigError(
                "player control requires the task command speed limits",
                code="CONFIG_MISSING_FIELD",
                path="controller",
            )
        return PlayerVelocityController(
            speed_mps=float(config.command.initial_speed_max),
            max_yaw_rate=float(config.max_yaw_rate),
            batch_size=batch_size,
        )
    if name == "fixed":
        if task.command_channels().get("velocity") != 3:
            raise ConfigError(
                "fixed playback requires a velocity command channel of width 3",
                code="CONFIG_OUT_OF_RANGE",
                path="controller",
            )
        if fixed_velocity is None or len(fixed_velocity) != 3:
            raise ConfigError(
                "fixed playback requires exactly three --fixed-velocity values",
                code="CONFIG_MISSING_FIELD",
                path="fixed_velocity",
            )
        if not all(math.isfinite(float(value)) for value in fixed_velocity):
            raise ConfigError(
                "fixed velocity values must be finite",
                code="CONFIG_OUT_OF_RANGE",
                path="fixed_velocity",
            )
        return FixedVelocityController(values=fixed_velocity, batch_size=batch_size)
    factory = _FACTORIES.get(name)
    if factory is None:
        raise ConfigError(
            f"unknown play controller {name!r}",
            code="CONFIG_OUT_OF_RANGE",
            path="controller",
        )
    return factory(task, batch_size)


def velocity_from_held_keys(
    held: Set[str],
    *,
    speed_mps: float,
    max_yaw_rate: float,
) -> torch.Tensor:
    """Map player keys onto the forward-and-yaw commands used in training."""

    normalized = {key.upper() for key in held}
    moving = "W" in normalized and "S" not in normalized
    left = "A" in normalized or "Q" in normalized
    right = "D" in normalized or "E" in normalized
    yaw = float(left) - float(right)
    return torch.tensor(
        [
            speed_mps if moving else 0.0,
            0.0,
            yaw * max_yaw_rate if moving else 0.0,
        ],
        dtype=torch.float32,
    )


def poll_held_keys() -> set[str]:
    """Read the OS keyboard so the UE viewport can keep focus."""

    if sys.platform != "win32":
        return set()
    import ctypes

    user32 = ctypes.windll.user32
    held: set[str] = set()
    for code, name in (
        (_KEY_W, "W"),
        (_KEY_A, "A"),
        (_KEY_S, "S"),
        (_KEY_D, "D"),
        (_KEY_Q, "Q"),
        (_KEY_E, "E"),
    ):
        if user32.GetAsyncKeyState(code) & 0x8000:
            held.add(name)
    return held


class PlayerVelocityController:
    """Publish trained velocity commands and hold the default pose when idle."""

    def __init__(self, *, speed_mps: float, max_yaw_rate: float, batch_size: int) -> None:
        self.speed_mps = speed_mps
        self.max_yaw_rate = max_yaw_rate
        self.batch_size = batch_size
        self._held: set[str] | None = None
        self._velocity = torch.zeros((batch_size, 3), dtype=torch.float32)

    def set_held(self, held: set[str] | None) -> None:
        """Install a key set for tests. ``None`` polls the OS on each update."""

        self._held = held

    def channels(self) -> Mapping[str, int]:
        return {"velocity": 3}

    def update(self, raw_state: Mapping[str, torch.Tensor]) -> None:
        del raw_state
        held = poll_held_keys() if self._held is None else self._held
        row = velocity_from_held_keys(
            held, speed_mps=self.speed_mps, max_yaw_rate=self.max_yaw_rate
        )
        self._velocity = row.reshape(1, 3).expand(self.batch_size, 3).clone()

    def reset(self, reset_mask: torch.Tensor, post_reset_state: Mapping[str, torch.Tensor]) -> None:
        del reset_mask
        self.update(post_reset_state)

    def current(self) -> Mapping[str, torch.Tensor]:
        return {"velocity": self._velocity}

    def gate_actions(self, actions: torch.Tensor) -> torch.Tensor:
        """Use default joint targets while no walking command is held."""

        idle = self._velocity.eq(0).all(dim=1).to(device=actions.device)
        return actions.masked_fill(idle.unsqueeze(1), 0.0)


class FixedVelocityController:
    """Publish one explicit velocity command for every step and reset."""

    def __init__(self, *, values: Sequence[float], batch_size: int) -> None:
        self._velocity = (
            torch.tensor(tuple(float(value) for value in values), dtype=torch.float32)
            .reshape(1, 3)
            .expand(batch_size, 3)
            .clone()
        )

    def channels(self) -> Mapping[str, int]:
        return {"velocity": 3}

    def update(self, raw_state: Mapping[str, torch.Tensor]) -> None:
        del raw_state

    def reset(self, reset_mask: torch.Tensor, post_reset_state: Mapping[str, torch.Tensor]) -> None:
        del reset_mask, post_reset_state

    def current(self) -> Mapping[str, torch.Tensor]:
        return {"velocity": self._velocity}

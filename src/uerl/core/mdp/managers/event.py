"""Event manager: startup / reset / interval terms via a Session bridge."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any, Literal, cast

import torch

from uerl.core.config.robot import RobotSpec
from uerl.core.mdp.lib.events import EventSessionBridge
from uerl.core.mdp.prepare import prepare_term_params
from uerl.core.mdp.terms import EventCfg, EventTermCfg
from uerl.errors import ConfigError

EventMode = Literal["startup", "reset", "interval"]
_VALID_MODES: frozenset[str] = frozenset({"startup", "reset", "interval"})


class _PreparedEventTerm:
    """One assembled event term with resolved params."""

    __slots__ = ("name", "func", "mode", "interval_range_s", "params")

    def __init__(
        self,
        name: str,
        func: Callable[..., None],
        mode: EventMode,
        interval_range_s: tuple[float, float] | None,
        params: dict[str, Any],
    ) -> None:
        self.name = name
        self.func = func
        self.mode = mode
        self.interval_range_s = interval_range_s
        self.params = params


class EventManager:
    """Apply named event terms at startup, reset, or per-Slot intervals.

    Terms never write UE or Task state directly. Callers bind an
    :class:`EventSessionBridge` so every mutation is staged for Session I/O.
    Interval timers are sampled independently per term and Slot from ``generator``.
    """

    def __init__(
        self,
        cfg: EventCfg,
        spec: RobotSpec,
        *,
        batch_size: int,
        device: str | torch.device,
        generator: torch.Generator,
    ) -> None:
        if batch_size < 1:
            raise ConfigError(
                "batch_size must be positive",
                code="CONFIG_OUT_OF_RANGE",
                path="batch_size",
            )
        if not cfg.terms:
            raise ConfigError(
                "EventCfg.terms must not be empty",
                code="CONFIG_OUT_OF_RANGE",
                path="terms",
            )
        self._device = torch.device(device)
        self._batch_size = int(batch_size)
        self._generator = generator
        self._bridge: EventSessionBridge | None = None
        prepared: list[_PreparedEventTerm] = []
        seen: set[str] = set()
        for name, term_cfg in cfg.terms.items():
            prepared.append(self._prepare_term(name, term_cfg, spec, seen))
        self._terms = tuple(prepared)
        by_mode: dict[str, list[_PreparedEventTerm]] = {mode: [] for mode in _VALID_MODES}
        for term in self._terms:
            by_mode[term.mode].append(term)
        self._by_mode = {mode: tuple(terms) for mode, terms in by_mode.items()}
        self._interval_countdowns = {
            term.name: torch.zeros(self._batch_size, dtype=torch.float32, device=self._device)
            for term in self._by_mode["interval"]
        }
        self._resample_intervals(torch.ones(self._batch_size, dtype=torch.bool, device=self._device))

    @staticmethod
    def _prepare_term(
        name: str,
        term_cfg: EventTermCfg,
        spec: RobotSpec,
        seen: set[str],
    ) -> _PreparedEventTerm:
        if not name:
            raise ConfigError(
                "event term names must be non-empty",
                code="CONFIG_OUT_OF_RANGE",
                path="terms",
            )
        if name in seen:
            raise ConfigError(
                f"duplicate event term name {name!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=f"terms.{name}",
            )
        seen.add(name)
        raw_mode = term_cfg.mode
        if raw_mode == "startup":
            typed_mode: EventMode = "startup"
        elif raw_mode == "reset":
            typed_mode = "reset"
        elif raw_mode == "interval":
            typed_mode = "interval"
        else:
            raise ConfigError(
                f"event mode must be one of {sorted(_VALID_MODES)}, got {raw_mode!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=f"terms.{name}.mode",
            )
        interval = term_cfg.interval_range_s
        if typed_mode == "interval":
            if interval is None:
                raise ConfigError(
                    "interval event terms require interval_range_s",
                    code="CONFIG_MISSING_FIELD",
                    path=f"terms.{name}.interval_range_s",
                )
            low, high = float(interval[0]), float(interval[1])
            if not (0.0 <= low <= high) or high <= 0.0:
                raise ConfigError(
                    "interval_range_s must satisfy 0 <= low <= high and high > 0",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"terms.{name}.interval_range_s",
                )
            interval = (low, high)
        elif interval is not None:
            raise ConfigError(
                "interval_range_s is only valid for interval mode",
                code="CONFIG_OUT_OF_RANGE",
                path=f"terms.{name}.interval_range_s",
            )
        func = cast(Callable[..., None], term_cfg.func)
        params = prepare_term_params(dict(term_cfg.params), spec)
        return _PreparedEventTerm(name, func, typed_mode, interval, params)

    def bind_bridge(self, bridge: EventSessionBridge) -> None:
        """Attach the Session-mediated sink used by ``apply``."""

        self._bridge = bridge

    @property
    def term_modes(self) -> dict[str, str]:
        """Return each term name and the mode that runs it."""

        return {term.name: term.mode for term in self._terms}

    @property
    def available_modes(self) -> frozenset[str]:
        """Return modes that have at least one registered term."""

        return frozenset(mode for mode, terms in self._by_mode.items() if terms)

    @property
    def next_interval_s(self) -> torch.Tensor:
        """Return the per-Slot countdown until any interval term fires (seconds)."""

        if not self._interval_countdowns:
            return torch.zeros(self._batch_size, dtype=torch.float32, device=self._device)
        return torch.stack(tuple(self._interval_countdowns.values())).amin(dim=0)

    def reset(self, mask: torch.Tensor) -> None:
        """Restart interval timers for Slots entering a new episode."""

        self._resample_intervals(mask.to(device=self._device, dtype=torch.bool).reshape(-1))

    def apply(
        self,
        mode: str,
        *,
        mask: torch.Tensor | None = None,
        dt: float | None = None,
    ) -> torch.Tensor | None:
        """Run every term registered for ``mode``.

        For ``interval``, ``dt`` advances per-Slot countdowns; only due Slots
        receive the term mask, then their next interval is resampled. Returns
        the due mask for interval mode, otherwise ``None``.
        """

        if mode not in _VALID_MODES:
            raise ConfigError(
                f"unknown event mode {mode!r}",
                code="CONFIG_OUT_OF_RANGE",
                path="mode",
            )
        terms = self._by_mode[mode]
        if not terms:
            return None
        bridge = self._bridge
        if bridge is None:
            raise RuntimeError("EventManager.apply requires bind_bridge(...) first")

        if mode == "interval":
            if dt is None or dt <= 0.0:
                raise ConfigError(
                    "interval apply requires a positive dt",
                    code="CONFIG_OUT_OF_RANGE",
                    path="dt",
                )
            any_due = torch.zeros(self._batch_size, dtype=torch.bool, device=self._device)
            for term in terms:
                countdown = self._interval_countdowns[term.name]
                countdown -= float(dt)
                due = countdown <= 0.0
                if mask is not None:
                    due = due & mask.to(device=self._device, dtype=torch.bool)
                if bool(due.any()):
                    term.func(bridge, due, generator=self._generator, **term.params)
                    self._resample_interval(term, due)
                any_due |= due
            return any_due

        active = (
            torch.ones(self._batch_size, dtype=torch.bool, device=self._device)
            if mask is None
            else mask.to(device=self._device, dtype=torch.bool).reshape(-1)
        )
        if active.numel() != self._batch_size:
            raise ConfigError(
                "event mask batch size does not match EventManager",
                code="CONFIG_OUT_OF_RANGE",
                path="mask",
            )
        if not bool(active.any()):
            return None
        for term in terms:
            term.func(bridge, active, generator=self._generator, **term.params)
        return None

    def _resample_intervals(self, mask: torch.Tensor) -> None:
        for term in self._by_mode["interval"]:
            self._resample_interval(term, mask)

    def _resample_interval(self, term: _PreparedEventTerm, mask: torch.Tensor) -> None:
        low, high = cast(tuple[float, float], term.interval_range_s)
        selected = torch.nonzero(mask.to(dtype=torch.bool).reshape(-1), as_tuple=False).flatten()
        if not selected.numel():
            return
        # Sample on CPU with the seed-derived generator, then copy to device.
        unit = torch.rand((int(selected.numel()),), generator=self._generator)
        samples = low + (high - low) * unit
        self._interval_countdowns[term.name][selected] = samples.to(device=self._device, dtype=torch.float32)


__all__ = ["EventManager", "EventMode"]

"""Reward manager: weight × term with per-term episode sums."""

from __future__ import annotations

import math
from collections.abc import Callable, Mapping
from typing import Any, cast

import torch

from uerl.core.config.robot import RobotSpec
from uerl.core.direct.types import StepContext, TerminationResult
from uerl.core.mdp.prepare import prepare_term_params
from uerl.core.mdp.terms import RewardCfg, RewTermCfg
from uerl.errors import ConfigError

# Episode metric keys match Isaac Lab's RewardManager extras prefix so training
# dashboards and ticket-28 PhantomX metric migration stay character-stable.
_EPISODE_REWARD_PREFIX = "Episode_Reward/"


class _PreparedRewardTerm:
    """One assembled reward term with resolved params."""

    __slots__ = ("name", "func", "weight", "params", "consumes_terminations", "time_mode")

    def __init__(
        self,
        name: str,
        func: Callable[..., torch.Tensor],
        weight: float,
        params: dict[str, Any],
        *,
        consumes_terminations: bool,
        time_mode: str,
    ) -> None:
        self.name = name
        self.func = func
        self.weight = weight
        self.params = params
        self.consumes_terminations = consumes_terminations
        self.time_mode = time_mode


class RewardManager:
    """Combine named reward terms as ``weight × value``.

    Transition terms apply once. Rate terms scale from their reference interval
    to the completed physical control interval in :class:`StepContext`.
    """

    def __init__(
        self,
        cfg: RewardCfg,
        spec: RobotSpec,
        *,
        batch_size: int,
        device: str | torch.device,
    ) -> None:
        if not cfg.terms:
            raise ConfigError(
                "RewardCfg.terms must not be empty",
                code="CONFIG_OUT_OF_RANGE",
                path="terms",
            )
        reference_dt_s = cfg.reference_dt_s
        if reference_dt_s is not None and (not math.isfinite(reference_dt_s) or reference_dt_s <= 0.0):
            raise ConfigError(
                "reward reference_dt_s must be finite and positive",
                code="CONFIG_OUT_OF_RANGE",
                path="reference_dt_s",
            )
        self._reference_dt_s = reference_dt_s
        prepared: list[_PreparedRewardTerm] = []
        seen: set[str] = set()
        for name, term_cfg in cfg.terms.items():
            if not name:
                raise ConfigError(
                    "reward term names must be non-empty",
                    code="CONFIG_OUT_OF_RANGE",
                    path="terms",
                )
            if name in seen:
                raise ConfigError(
                    f"duplicate reward term name {name!r}",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"terms.{name}",
                )
            seen.add(name)
            prepared.append(_prepare_term(name, term_cfg, spec))
        if reference_dt_s is None and any(term.time_mode == "rate" for term in prepared):
            raise ConfigError(
                "rate reward requires reference_dt_s",
                code="CONFIG_OUT_OF_RANGE",
                path="reference_dt_s",
            )
        self._terms = tuple(prepared)
        self.capture_step_values = False
        self.debug_step_values: Mapping[str, torch.Tensor] | None = None
        self._term_names = tuple(term.name for term in self._terms)
        torch_device = torch.device(device)
        self._episode_sums = {
            name: torch.zeros(batch_size, device=torch_device) for name in self._term_names
        }

    @property
    def term_names(self) -> tuple[str, ...]:
        return self._term_names

    def term_step_values(
        self,
        ctx: StepContext,
        terminations: TerminationResult,
    ) -> Mapping[str, torch.Tensor]:
        """Return per-term ``weight × value`` for the current step."""

        values: dict[str, torch.Tensor] = {}
        for term in self._terms:
            kwargs = dict(term.params)
            if term.consumes_terminations:
                kwargs["terminations"] = terminations
            value = term.func(ctx, **kwargs) * term.weight
            if term.time_mode == "rate":
                if self._reference_dt_s is None or ctx.transition_dt is None:
                    raise ValueError("rate reward requires reference_dt_s and transition_dt")
                value = value * (ctx.transition_dt / self._reference_dt_s)
            values[term.name] = value
        return values

    def compute(
        self,
        ctx: StepContext,
        terminations: TerminationResult,
    ) -> torch.Tensor:
        """Return per-row total reward; accumulate episode sums for logging."""

        rows = ctx.episode_steps.shape[0]
        parts = self.term_step_values(ctx, terminations)
        self.debug_step_values = parts if self.capture_step_values else None
        total = torch.zeros(rows, device=ctx.episode_steps.device, dtype=torch.float32)
        slot_ids = ctx.slot_ids
        if slot_ids is None:
            slot_ids = torch.arange(rows, device=ctx.episode_steps.device)
        for name, value in parts.items():
            self._episode_sums[name][slot_ids] += value
            total = total + value
        return total

    def episode_log(self, reset_mask: torch.Tensor) -> Mapping[str, float]:
        """Mean per-term episode sum over rows marked finished (pre-reset)."""

        if reset_mask.dtype != torch.bool:
            raise ConfigError(
                "reset_mask must be a boolean tensor",
                code="CONFIG_OUT_OF_RANGE",
                path="reset_mask",
            )
        if not bool(reset_mask.any()):
            return {}
        finished = {
            f"{_EPISODE_REWARD_PREFIX}{name}": float(sums[reset_mask].mean().item())
            for name, sums in self._episode_sums.items()
        }
        return finished

    def reset(self, mask: torch.Tensor) -> None:
        """Zero episode sums for rows selected by ``mask`` only."""

        if mask.dtype != torch.bool:
            raise ConfigError(
                "reset mask must be a boolean tensor",
                code="CONFIG_OUT_OF_RANGE",
                path="mask",
            )
        for sums in self._episode_sums.values():
            sums[mask] = 0.0


def _prepare_term(name: str, term_cfg: RewTermCfg, spec: RobotSpec) -> _PreparedRewardTerm:
    params = prepare_term_params(dict(term_cfg.params), spec)
    func = cast(Callable[..., torch.Tensor], term_cfg.func)
    return _PreparedRewardTerm(
        name=name,
        func=func,
        weight=float(term_cfg.weight),
        params=params,
        consumes_terminations=bool(term_cfg.consumes_terminations),
        time_mode=term_cfg.time_mode,
    )


__all__ = ["RewardManager"]

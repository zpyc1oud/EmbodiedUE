"""Termination manager wrapping ``combine_termination_terms``."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any, cast

import torch

from uerl.core.config.robot import RobotSpec
from uerl.core.direct.termination import TerminationTerm, combine_termination_terms
from uerl.core.direct.types import StepContext, TerminationResult
from uerl.core.mdp.prepare import prepare_term_params
from uerl.core.mdp.terms import TerminationCfg
from uerl.errors import ConfigError


class TerminationManager:
    """Evaluate parameterized termination terms via the existing combiner."""

    def __init__(self, cfg: TerminationCfg, spec: RobotSpec) -> None:
        if not cfg.terms:
            raise ConfigError(
                "TerminationCfg.terms must not be empty",
                code="CONFIG_OUT_OF_RANGE",
                path="terms",
            )
        terms: list[TerminationTerm] = []
        for name, term_cfg in cfg.terms.items():
            if not name:
                raise ConfigError(
                    "termination term names must be non-empty",
                    code="CONFIG_OUT_OF_RANGE",
                    path="terms",
                )
            params = prepare_term_params(dict(term_cfg.params), spec)
            func = cast(Callable[..., torch.Tensor], term_cfg.func)

            def _evaluate(
                context: StepContext,
                *,
                _func: Callable[..., torch.Tensor] = func,
                _params: dict[str, Any] = params,
            ) -> torch.Tensor:
                return _func(context, **_params)

            terms.append(
                TerminationTerm(
                    name=name,
                    time_out=term_cfg.time_out,
                    evaluate=_evaluate,
                )
            )
        self._terms = tuple(terms)
        self._term_names = tuple(cfg.terms)

    @property
    def term_names(self) -> tuple[str, ...]:
        return self._term_names

    def compute(self, ctx: StepContext) -> TerminationResult:
        """Retain combiner semantics: failures vs timeouts and per-term reasons."""

        return combine_termination_terms(self._terms, ctx)


__all__ = ["TerminationManager"]

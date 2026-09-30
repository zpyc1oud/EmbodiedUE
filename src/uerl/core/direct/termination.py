"""Compose named task termination terms with Gymnasium lifecycle semantics."""

from __future__ import annotations

from collections.abc import Callable, Iterable
from dataclasses import dataclass

import torch

from .types import StepContext, TerminationResult

TerminationEvaluator = Callable[[StepContext], torch.Tensor]


@dataclass(frozen=True, slots=True)
class TerminationTerm:
    """Declare one named boolean termination term."""

    name: str
    time_out: bool
    evaluate: TerminationEvaluator


def combine_termination_terms(
    terms: Iterable[TerminationTerm], context: StepContext
) -> TerminationResult:
    """Evaluate named terms and split failures from timeouts."""

    declared = tuple(terms)
    names: set[str] = set()
    terminated = torch.zeros(context.episode_steps.shape, dtype=torch.bool, device=context.episode_steps.device)
    truncated = torch.zeros_like(terminated)
    reason: dict[str, torch.Tensor] = {}
    for term in declared:
        if not term.name:
            raise ValueError("termination term names must be non-empty")
        if term.name in names:
            raise ValueError(f"termination term '{term.name}' is declared more than once")
        names.add(term.name)
        value = term.evaluate(context)
        if value.shape != terminated.shape:
            message = (
                f"termination term '{term.name}' returned shape {tuple(value.shape)}, "
                + f"expected {tuple(terminated.shape)}"
            )
            raise ValueError(message)
        value = value.to(device=terminated.device, dtype=torch.bool)
        reason[term.name] = value
        if term.time_out:
            truncated |= value
        else:
            terminated |= value
    return TerminationResult(terminated=terminated, truncated=truncated, reason=reason)


__all__ = ["TerminationEvaluator", "TerminationTerm", "combine_termination_terms"]

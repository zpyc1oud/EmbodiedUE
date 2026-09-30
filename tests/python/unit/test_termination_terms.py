"""Verify named termination composition and timeout classification."""

from __future__ import annotations

import pytest
import torch

from uerl.core.direct.termination import TerminationTerm, combine_termination_terms
from uerl.core.direct.types import PhysicalCommandBatch, StepContext


def _context(steps: torch.Tensor) -> StepContext:
    rows = steps.shape[0]
    return StepContext(
        raw_state={},
        previous_policy_actions=torch.zeros(rows, 1),
        policy_actions=torch.zeros(rows, 1),
        physical_command=PhysicalCommandBatch({}),
        transition_state={},
        state_valid=torch.ones(rows, dtype=torch.bool),
        slot_fault_code=torch.zeros(rows, dtype=torch.uint16),
        episode_steps=steps,
    )


def test_named_terms_split_failures_and_timeouts_and_preserve_reasons() -> None:
    context = _context(torch.arange(3))
    result = combine_termination_terms(
        (
            TerminationTerm("failure", False, lambda current: current.episode_steps.eq(1)),
            TerminationTerm("timeout", True, lambda current: current.episode_steps.ge(2)),
        ),
        context,
    )

    assert torch.equal(result.terminated, torch.tensor([False, True, False]))
    assert torch.equal(result.truncated, torch.tensor([False, False, True]))
    assert torch.equal(result.reason["failure"], result.terminated)
    assert torch.equal(result.reason["timeout"], result.truncated)


def test_named_terms_merge_multiple_terms_in_the_same_lifecycle_mask() -> None:
    context = _context(torch.arange(2))
    result = combine_termination_terms(
        (
            TerminationTerm(
                "failure_a", False, lambda current: torch.ones_like(current.episode_steps, dtype=torch.bool)
            ),
            TerminationTerm("failure_b", False, lambda current: current.episode_steps.eq(1)),
            TerminationTerm(
                "timeout", True, lambda current: torch.ones_like(current.episode_steps, dtype=torch.bool)
            ),
        ),
        context,
    )

    assert torch.equal(result.terminated, torch.ones(2, dtype=torch.bool))
    assert torch.equal(result.truncated, torch.ones(2, dtype=torch.bool))


def test_named_terms_reject_duplicate_names_and_wrong_shapes() -> None:
    context = _context(torch.zeros(2, dtype=torch.long))
    with pytest.raises(ValueError, match="declared more than once"):
        combine_termination_terms(
            (
                TerminationTerm("duplicate", False, lambda current: current.episode_steps.bool()),
                TerminationTerm("duplicate", True, lambda current: current.episode_steps.bool()),
            ),
            context,
        )

    with pytest.raises(ValueError, match="returned shape"):
        combine_termination_terms(
            (TerminationTerm("wrong_shape", False, lambda current: torch.zeros(1, dtype=torch.bool)),),
            context,
        )

"""Define typed batches exchanged by the framework-independent Direct runtime."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any, cast

import torch


def _readonly_mapping(values: Mapping[str, torch.Tensor]) -> Mapping[str, torch.Tensor]:
    """Prevent replacement of named fields after a batch crosses the seam."""

    return MappingProxyType(dict(values))


def _freeze_schema_value(value: Any) -> Any:
    """Freeze JSON-like schema metadata without validating trusted Task values."""

    if isinstance(value, Mapping):
        return MappingProxyType({str(key): _freeze_schema_value(item) for key, item in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_freeze_schema_value(item) for item in value)
    return value


def _thaw_schema_value(value: Any) -> Any:
    """Convert frozen schema metadata back to JSON-compatible containers."""

    if isinstance(value, Mapping):
        return {str(key): _thaw_schema_value(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw_schema_value(item) for item in value]
    return value


@dataclass(frozen=True, slots=True)
class PhysicalCommandBatch:
    """Store named physical commands produced by one DirectTask step.

    Attributes:
        values: A fixed mapping from negotiated command names to batch-first
            tensors. The mapping cannot be replaced after construction, while
            the tensors remain the caller-owned objects.
    """

    values: Mapping[str, torch.Tensor]

    def __post_init__(self) -> None:
        object.__setattr__(self, "values", _readonly_mapping(self.values))

    def __getitem__(self, name: str) -> torch.Tensor:
        """Return one named command field.

        Args:
            name: A command field name declared by the Task action schema.

        Returns:
            The tensor stored under ``name``; this is the original tensor view,
            not a defensive clone.

        Raises:
            KeyError: If the name is not present in the negotiated command map.
        """

        return self.values[name]


@dataclass(frozen=True, slots=True)
class StateBatch:
    """Store one Worker state batch plus validity, fault, and episode metadata.

    Attributes:
        values: Fixed named state fields in stable Slot order. Tensor contents
            are not cloned by this DTO.
        state_valid: Boolean validity for each Slot row.
        slot_fault_code: Worker fault code for each Slot row; zero means no
            reported fault.
        episode_index: Worker episode index for each Slot row.
    """

    values: Mapping[str, torch.Tensor]
    state_valid: torch.Tensor
    slot_fault_code: torch.Tensor
    episode_index: torch.Tensor

    def __post_init__(self) -> None:
        object.__setattr__(self, "values", _readonly_mapping(self.values))

    def __getitem__(self, name: str) -> torch.Tensor:
        """Return one named state field.

        Args:
            name: A state field name declared by the Task schema.

        Returns:
            The tensor stored under ``name`` without cloning it.

        Raises:
            KeyError: If the requested field is absent.
        """

        return self.values[name]


@dataclass(frozen=True, slots=True)
class InitialState(StateBatch):
    """Store the full-batch State returned by the Initialize transaction.

    The rows establish the baseline before the Session enters Ready; no reset
    or transition has been applied by the Direct environment.
    """


@dataclass(frozen=True, slots=True)
class TransitionState(StateBatch):
    """Store the State returned by one Step before the environment applies Reset.

    Task termination and reward mathematics must use these pre-reset values,
    including values from rows that will be reset before the next observation.
    """


@dataclass(frozen=True, slots=True)
class PostResetState(StateBatch):
    """Store the State returned after the environment resets selected Slots.

    Values for selected rows describe the new episode, while unselected rows
    retain their current episode and remain aligned with the full Slot batch.
    """


@dataclass(frozen=True, slots=True)
class TerminationResult:
    """Store task-owned termination and timeout masks for a batch.

    Attributes:
        terminated: Boolean task termination mask, one value per compact valid
            context row.
        truncated: Boolean timeout or external-limit mask in the same row order.
        reason: Optional metric-like mapping explaining the task decision; it is
            mapping-immutable after construction but its tensor values are not
            cloned.
    """

    terminated: torch.Tensor
    truncated: torch.Tensor
    reason: Mapping[str, Any] = field(default_factory=lambda: MappingProxyType({}))

    def __post_init__(self) -> None:
        object.__setattr__(self, "reason", MappingProxyType(dict(self.reason)))


@dataclass(frozen=True, slots=True)
class StepContext:
    """Provide transition data to pure, batch-oriented Task methods.

    All tensors use the compact valid-row order selected by the Direct
    environment. ``transition_state`` is pre-reset data; Task methods must not
    infer that a row has already been reset.

    Attributes:
        raw_state: Named state immediately before the Worker step.
        previous_policy_actions: Policy output used for the preceding transition.
        policy_actions: Policy output aligned with the compact rows.
        physical_command: Physical commands generated for those rows.
        transition_state: Named pre-reset state returned by the Worker step.
        state_valid: Validity mask for the compact rows.
        slot_fault_code: Fault metadata retained for diagnostics.
        episode_steps: Python episode lengths after the transition.
        transition_dt: Physical seconds completed by this transition.
        slot_ids: Stable Slot IDs for compact rows, or ``None`` for direct callers.
        episode_elapsed_s: Per-row simulated seconds including the completed transition.
    """

    raw_state: Mapping[str, torch.Tensor]
    previous_policy_actions: torch.Tensor
    policy_actions: torch.Tensor
    physical_command: PhysicalCommandBatch
    transition_state: Mapping[str, torch.Tensor]
    state_valid: torch.Tensor
    slot_fault_code: torch.Tensor
    episode_steps: torch.Tensor
    transition_dt: float | None = None
    slot_ids: torch.Tensor | None = None
    episode_elapsed_s: torch.Tensor | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "raw_state", _readonly_mapping(self.raw_state))
        object.__setattr__(self, "transition_state", _readonly_mapping(self.transition_state))


@dataclass(frozen=True, slots=True)
class SessionSchema:
    """Store the schema request sent by a Direct environment during initialization.

    Attributes:
        state_requirements: Ordered typed descriptors for raw Worker state fields.
        action_schema: Ordered typed descriptors for physical command fields.
            Nested mappings and sequences are frozen so the request identity
            cannot change after negotiation begins.
    """

    state_requirements: tuple[Mapping[str, Any], ...]
    action_schema: tuple[Mapping[str, Any], ...]

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "state_requirements",
            tuple(_freeze_schema_value(item) for item in self.state_requirements),
        )
        object.__setattr__(self, "action_schema", tuple(_freeze_schema_value(item) for item in self.action_schema))

    def as_request(self) -> dict[str, list[dict[str, Any]]]:
        """Return the JSON-compatible schema request owned by the Task.

        Returns:
            A fresh mutable JSON-compatible copy containing the two ordered
            descriptor arrays. Mutating the returned value does not change this
            frozen schema object.
        """

        state_requirements = [_thaw_schema_value(item) for item in self.state_requirements]
        action_schema = [_thaw_schema_value(item) for item in self.action_schema]
        return {
            "state_requirements": cast(list[dict[str, Any]], state_requirements),
            "action_schema": cast(list[dict[str, Any]], action_schema),
        }


__all__ = [
    "InitialState",
    "PhysicalCommandBatch",
    "PostResetState",
    "SessionSchema",
    "StateBatch",
    "StepContext",
    "TerminationResult",
    "TransitionState",
]

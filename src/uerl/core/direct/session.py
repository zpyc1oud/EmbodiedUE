"""Define the typed Session seam consumed by the framework-independent Direct environment."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any, Protocol

import torch

from ..config.robot import RobotSpec
from ..mdp.lib.events import EventEffect
from .robot_observation import ObservationShapeTable
from .types import InitialState, PhysicalCommandBatch, PostResetState, SessionSchema, TransitionState


class DirectSession(Protocol):
    """Define the typed Worker operations required by ``UERLDirectEnv``.

    Implementations expose one stable batch with ``num_slots`` rows. Lifecycle
    calls follow Initialize, Ready, initial Reset, Step/Reset, and Close order;
    the environment owns the decision of which rows to reset.
    """

    num_slots: int

    @property
    def descriptor(self) -> Mapping[str, Any]:
        """Return the UE Describe response used for shape negotiation."""

        ...

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest Step."""

        ...

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest Reset."""

        ...

    def initialize_with_robot_spec(
        self,
        schema: SessionSchema,
        bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
    ) -> InitialState:
        """Describe topology, bind UE shapes, and commit one Task schema."""

        ...

    def acknowledge_ready(self) -> None:
        """Enter the Worker Ready state after initialization has been persisted.

        Raises:
            SessionError: If Initialize has not completed or Ready cannot be
                acknowledged.

        Side effects:
            Persist the Ready lifecycle transition. Call this exactly once before
            Step or Reset.
        """

        ...

    def step(self, command: PhysicalCommandBatch, step_decimation: int | None = None) -> TransitionState:
        """Advance all Slots by one synchronized Worker step without performing Reset.

        Args:
            command: Full-batch physical commands in the negotiated field names
                and stable Slot order.
            step_decimation: One positive int32 count for this synchronized
                Step; fixed ranges may omit it.

        Returns:
            The pre-reset Transition State for every Slot, including validity,
            fault codes, and episode metadata produced by the Worker.

        Raises:
            SessionError: If the Session is not Ready or the Worker rejects the
                command or exchange.

        Side effects:
            Advance Worker simulation exactly once. This method never performs
            automatic reset, even when rows become terminal.
        """

        ...

    def apply_event(self, effect: EventEffect) -> Mapping[str, Any]:
        """Apply one Session-mediated event effect while the Worker is Ready.

        Args:
            effect: Full-batch values plus a boolean Slot mask produced by an
                EventManager term.

        Side effects:
            Mutate only the selected Worker Slots through the event protocol;
            no Task or direct UE state is written by the environment.
        """

        ...

    def reset(
        self,
        reset_mask: torch.Tensor,
        terrain_levels: torch.Tensor | None = None,
        reset_values: torch.Tensor | None = None,
    ) -> PostResetState:
        """Reset selected Slots, optionally selecting their terrain levels.

        Args:
            reset_mask: Boolean tensor with one element per Slot. True rows are
                reset; false rows retain their current episode state.
            terrain_levels: Optional uint16-compatible full-batch level vector.
                Selected rows are reset onto these pre-generated terrain tiers.
            reset_values: Optional full-batch robot reset values staged by an
                EventManager reset term. When supplied, selected rows use these
                values instead of sampling a second reset distribution.

        Returns:
            Post-Reset State aligned with the full Slot batch. The returned
            validity and episode index describe the resulting rows.

        Raises:
            SessionError: If the Session is not Ready, the mask shape is invalid,
                or the Worker response does not match the requested mask.

        Side effects:
            Reset only selected Worker Slots and advance their episode indices.
            Calling with an all-false mask is still a protocol exchange.
        """

        ...

    def close(self, reason: str = "session_close") -> None:
        """Close the typed Session and its owned resources exactly once.

        Args:
            reason: Audit text used by the underlying ownership and transport
                cleanup path.

        Side effects:
            Release owned resources without retrying or reconnecting. Repeated
            calls must be idempotent.
        """

        ...


__all__ = ["DirectSession"]

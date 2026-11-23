"""Pure tensor and tracing helpers for Direct environment orchestration."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, cast

import torch

from .types import PhysicalCommandBatch, StateBatch, StepContext


def compact_context(
    previous_state: StateBatch,
    transition: StateBatch,
    physical_command: PhysicalCommandBatch,
    previous_policy_actions: torch.Tensor,
    policy_actions: torch.Tensor,
    transition_dt: float,
    episode_steps: torch.Tensor,
    episode_elapsed_s: torch.Tensor,
    valid_ids: torch.Tensor,
) -> StepContext:
    """Select only valid rows before calling Task mathematics."""

    return StepContext(
        raw_state=select_mapping(previous_state.values, valid_ids),
        previous_policy_actions=previous_policy_actions.index_select(0, valid_ids),
        policy_actions=policy_actions.index_select(0, valid_ids),
        physical_command=PhysicalCommandBatch(select_mapping(physical_command.values, valid_ids)),
        transition_state=select_mapping(transition.values, valid_ids),
        state_valid=transition.state_valid[valid_ids],
        slot_fault_code=transition.slot_fault_code[valid_ids],
        episode_steps=episode_steps[valid_ids],
        episode_elapsed_s=episode_elapsed_s[valid_ids],
        transition_dt=transition_dt,
        slot_ids=valid_ids,
    )


def planar_command(
    command_velocity: torch.Tensor | None,
    num_envs: int,
    device: torch.device,
) -> torch.Tensor:
    """Return the published body-frame planar command, or zeros when none is published."""

    if command_velocity is None:
        return torch.zeros((num_envs, 2), device=device, dtype=torch.float32)
    if command_velocity.ndim != 2 or command_velocity.shape[0] != num_envs or command_velocity.shape[1] < 2:
        raise ValueError(
            f"published velocity must have shape ({num_envs}, >=2), got {tuple(command_velocity.shape)}"
        )
    return command_velocity[:, :2].to(device=device, dtype=torch.float32).clone()


def merge_metadata_rows(
    current: torch.Tensor,
    replacement: torch.Tensor,
    mask: torch.Tensor,
) -> torch.Tensor:
    """Merge Worker unsigned metadata without relying on unavailable where kernels."""

    if current.dtype in (torch.uint16, torch.uint64):
        merged = [
            replacement_value if selected else current_value
            for current_value, replacement_value, selected in zip(
                current.tolist(), replacement.tolist(), mask.tolist(), strict=True
            )
        ]
        return torch.tensor(merged, dtype=current.dtype, device=current.device)
    return torch.where(mask, replacement, current)


def select_mapping(values: Mapping[str, torch.Tensor], indexes: torch.Tensor) -> dict[str, torch.Tensor]:
    """Select the same Slot rows from each named Tensor."""

    return {name: value[indexes] for name, value in values.items()}


def clone_mapping(values: Mapping[str, torch.Tensor]) -> dict[str, torch.Tensor]:
    """Copy a named batch before sparse reset merge or terminal capture."""

    return {name: value.clone() for name, value in values.items()}


def clone_observations(values: Mapping[str, torch.Tensor]) -> dict[str, torch.Tensor]:
    """Copy Task observation groups so terminal data cannot alias live state."""

    return {name: value.clone() for name, value in values.items()}


def trace_tensor_row(value: torch.Tensor, slot_index: int) -> list[bool | float | int]:
    """Copy one batch row into YAML-safe scalars only when tracing is enabled."""

    return cast(list[bool | float | int], value[slot_index].detach().cpu().reshape(-1).tolist())


def trace_tensor_mapping(
    values: Mapping[str, torch.Tensor],
    slot_index: int,
) -> dict[str, list[bool | float | int]]:
    """Copy named tensor fields for one traced Slot."""

    return {name: trace_tensor_row(value, slot_index) for name, value in values.items()}


def trace_row_value(value: object, slot_index: int) -> object:
    """Select one Slot from termination details and convert tensors to YAML values."""

    if isinstance(value, torch.Tensor):
        if value.ndim == 0:
            return value.item()
        return value[slot_index].detach().cpu().reshape(-1).tolist()
    if isinstance(value, Mapping):
        return {str(name): trace_row_value(item, slot_index) for name, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [trace_row_value(item, slot_index) for item in value]
    return value


def expand_mapping(
    values: Mapping[str, Any],
    valid_ids: torch.Tensor,
    num_envs: int,
    device: torch.device,
) -> dict[str, Any]:
    """Scatter per-valid-row metrics or reasons back to the full Slot batch."""

    expanded: dict[str, Any] = {}
    for name, value in values.items():
        if not isinstance(value, torch.Tensor) or value.ndim == 0:
            expanded[name] = value
            continue
        full = torch.zeros(
            (num_envs, *value.shape[1:]),
            dtype=value.dtype,
            device=device,
        )
        full[valid_ids] = value.to(device=device)
        expanded[name] = full
    return expanded

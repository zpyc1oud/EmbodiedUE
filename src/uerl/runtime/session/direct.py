"""Adapt the raw Session boundary to the typed DirectSession seam."""

from __future__ import annotations

import math
import time
from collections.abc import Callable, Mapping
from typing import Any, Protocol, cast

import numpy as np
import torch

from ...core.codec import BatchCodec, InitializeDescription, InitializeResult, Layout, Segment
from ...core.config import ResolvedRunConfig
from ...core.config.robot import RobotSpec
from ...core.direct.robot_observation import ObservationShapeTable
from ...core.direct.types import InitialState, PhysicalCommandBatch, PostResetState, SessionSchema, TransitionState
from ...core.mdp.lib.events import EventEffect, sample_robot_reset_distributions
from ...errors import SessionError


class _RawSession(Protocol):
    """Define the small raw Session surface required by the adapter."""

    config: ResolvedRunConfig

    @property
    def described_robot_spec(self) -> RobotSpec | None:
        """Return the RobotSpec produced by the required Describe phase."""

        ...

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        """Return exclusive timings for the latest raw Step exchange."""

        ...

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        """Return exclusive timings for the latest raw Reset exchange."""

        ...

    def initialize(self, schema_request: Mapping[str, Any]) -> InitializeResult:
        """Run the Session Initialize transaction."""

        ...

    def describe(self, schema_request: Mapping[str, Any]) -> InitializeDescription:
        """Run the required topology description phase."""

        ...

    @property
    def descriptor(self) -> Mapping[str, Any]:
        """Return the last UE Initialize description DTO."""

        ...

    def acknowledge_ready(self) -> Mapping[str, Any]:
        """Pass the persisted Manifest hash to the Worker."""

        ...

    def step(self, layout_id: int, payload: bytes) -> tuple[Any, bytes]:
        """Exchange one raw Step frame."""

        ...

    def reset(self, layout_id: int, payload: bytes) -> tuple[Any, bytes]:
        """Exchange one raw Reset frame."""

        ...

    def event(self, request: Mapping[str, Any]) -> Mapping[str, Any]:
        """Exchange one raw event request while the Worker is Ready."""

        ...

    def close(self, reason: str = "session_close") -> None:
        """Close the raw Session."""

        ...


_NUMPY_DTYPES = {
    "float32": np.dtype("<f4"),
    "int32": np.dtype("<i4"),
    "uint8": np.dtype("u1"),
    "uint16": np.dtype("<u2"),
    "uint64": np.dtype("<u8"),
}


class UERLSessionAdapter:
    """Map negotiated named binary batches to typed Direct DTOs once.

    The raw Session owns wire payload validation. This adapter owns the typed
    Task-schema/Layout compatibility check, segment mapping, and copying into
    the requested Torch device while preserving full Slot alignment.
    """

    def __init__(self, session: _RawSession, *, device: str | torch.device = "cpu") -> None:
        """Bind the adapter to one raw Session and destination device.

        Args:
            session: A raw Session whose Worker configuration fixes the Slot
                count and whose lifecycle is owned by this adapter.
            device: Torch device receiving decoded state and metadata tensors.

        Side effects:
            Allocate the episode-index buffer. The raw Session is not initialized
            until ``initialize`` is called.
        """

        self._session = session
        self._device = torch.device(device)
        self.num_slots = session.config.worker.slot_count
        self._layouts: dict[str, Layout] = {}
        self._schema: SessionSchema | None = None
        self._initial_state: InitialState | None = None
        self._robot_spec: RobotSpec | None = None
        self._terrain_config = session.config.worker.terrain_config
        self._terrain_enabled = bool(self._terrain_config)
        self._terrain_levels = torch.zeros(self.num_slots, dtype=torch.uint16, device=self._device)
        self._episode_index = torch.zeros(self.num_slots, dtype=torch.uint64, device=self._device)
        self._closed = False
        self._last_step_timing: dict[str, float] = {}
        self._last_reset_timing: dict[str, float] = {}

    @property
    def initial_state(self) -> InitialState:
        """Return the decoded Worker state before DirectEnv performs its first reset."""

        if self._initial_state is None:
            raise SessionError("initial state is unavailable before Initialize")
        return self._initial_state

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest typed Step."""

        return self._last_step_timing

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        """Return exclusive client timings for the latest typed Reset."""

        return self._last_reset_timing

    @property
    def descriptor(self) -> Mapping[str, Any]:
        """Return the UE Initialize description used for shape negotiation."""

        return self._session.descriptor

    def initialize_with_robot_spec(
        self,
        schema: SessionSchema,
        bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
    ) -> InitialState:
        """Describe topology, bind Task semantics, then commit one schema."""

        description = self._session.describe(schema.as_request())
        robot_spec = self._session.described_robot_spec
        if robot_spec is None:
            raise SessionError("Worker description did not produce a RobotSpec")
        observation_shapes = ObservationShapeTable.from_descriptor(description.response)
        bound_schema = bind_robot_spec(robot_spec, observation_shapes)
        self._robot_spec = robot_spec
        result = self._session.initialize(bound_schema.as_request())
        return self._finish_initialize(bound_schema, result)

    def _finish_initialize(self, schema: SessionSchema, result: InitializeResult) -> InitialState:
        """Cache the committed layouts and decode its initial batch."""

        self._schema = schema
        response = result.response
        self._layouts = {
            descriptor["layout_kind"]: _trusted_layout(descriptor)
            for descriptor in cast(list[dict[str, Any]], response["layouts"])
        }
        self._validate_layout_contract()
        values, state_valid, fault_code, episode_index = self._decode_state(
            self._layouts["initial_state"],
            result.initial_payload,
        )
        self._episode_index = episode_index
        self._initial_state = InitialState(values, state_valid, fault_code, episode_index)
        return self._initial_state

    def acknowledge_ready(self) -> None:
        """Enter the Worker Ready state after the Manifest is persisted.

        Raises:
            SessionError: If the raw Session is not initialized or the Ready
                acknowledgement fails.

        Side effects:
            Forward the persisted Manifest identity through the raw Session.
            Call exactly once between ``initialize`` and ``step``/``reset``.
        """

        self._session.acknowledge_ready()

    def step(self, command: PhysicalCommandBatch, step_decimation: int | None = None) -> TransitionState:
        """Encode one named command batch and decode the Worker transition.

        Args:
            command: Full-batch named physical commands in the negotiated field
                names and stable Slot order.
            step_decimation: One positive int32 physics-frame count shared by
                all Slots. A fixed ``[N, N]`` configuration may omit it; a
                variable range must supply it explicitly.

        Returns:
            A TransitionState whose values, validity, and fault codes are copied
            from the Worker response. The episode-index vector remains the last
            known full-batch index until Reset returns updated indices.

        Raises:
            SessionError: If the command cannot satisfy the negotiated layout
                or the raw Session rejects the Step exchange.

        Side effects:
            Advance the Worker exactly once and replace no rows locally; the
            Direct environment decides whether and when to reset rows.
        """

        layout = self._layouts["step_action"]
        configured_range = getattr(self._session.config.worker, "decimation", (1, 1))
        if (
            step_decimation is None
            and isinstance(configured_range, tuple)
            and len(configured_range) == 2
            and configured_range[0] == configured_range[1]
        ):
            step_decimation = configured_range[0]
        if (
            type(step_decimation) is not int
            or not isinstance(configured_range, tuple)
            or len(configured_range) != 2
            or step_decimation < configured_range[0]
            or step_decimation > configured_range[1]
        ):
            raise SessionError("step_decimation is missing or outside the configured range")
        encode_start = time.perf_counter()
        encoded = self._encode_commands(layout, command, step_decimation)
        action_encode_s = time.perf_counter() - encode_start
        _header, payload = self._session.step(layout.layout_id, encoded)
        decode_start = time.perf_counter()
        values, state_valid, fault_code, _episode_index = self._decode_state(
            self._layouts["step_result"],
            payload,
        )
        state_decode_s = time.perf_counter() - decode_start
        self._last_step_timing = {
            "client_action_encode": action_encode_s,
            **self._session.last_step_timing,
            "client_state_decode": state_decode_s,
        }
        return TransitionState(values, state_valid, fault_code, self._episode_index.clone())

    def apply_event(self, effect: EventEffect) -> Mapping[str, Any]:
        """Encode one typed event effect and forward it to the raw Session."""

        mask = effect.mask.detach().to(device="cpu", dtype=torch.bool).reshape(-1)
        if mask.numel() != self.num_slots:
            raise SessionError("event mask must contain one value per Slot")
        values = effect.values.detach().to(device="cpu", dtype=torch.float32)
        if values.ndim != 2 or values.shape[0] != self.num_slots:
            raise SessionError("event values must be a full-batch matrix")
        if not bool(torch.isfinite(values).all()):
            raise SessionError("event values must be finite")
        expected_width = {"ground_friction": 2, "terrain_tier_params": 1, "root_push": 3}.get(
            effect.kind
        )
        if expected_width is None or values.shape[1] != expected_width:
            raise SessionError(f"event {effect.kind!r} has an unsupported value shape")
        selected = torch.nonzero(mask, as_tuple=False).flatten()
        response = self._session.event(
            {
                "kind": effect.kind,
                "slot_ids": selected.tolist(),
                "values": values[selected].tolist(),
            }
        )
        if response.get("applied") is not True or response.get("kind") != effect.kind:
            raise SessionError(f"Worker rejected event {effect.kind!r}")
        return response

    def reset(
        self,
        reset_mask: torch.Tensor,
        terrain_levels: torch.Tensor | None = None,
        reset_values: torch.Tensor | None = None,
    ) -> PostResetState:
        """Reset selected Slots and decode their Post-Reset State.

        Args:
            reset_mask: Boolean tensor with one element per Slot. The request is
                encoded as the negotiated little-endian bitset.
            terrain_levels: Optional full-batch terrain levels. Selected rows are
                placed on the requested pre-generated tiers.

        Returns:
            A full-batch PostResetState. Selected rows contain the new episode;
            unselected rows remain aligned with their existing episode.

        Raises:
            SessionError: If the response bitset contains unused bits or differs
                from the request mask.

        Side effects:
            Reset exactly the requested Worker rows, update the local episode
            indices, and close the raw Session on a reset-mask contract failure.
        """

        if reset_mask.shape != (self.num_slots,):
            raise SessionError("reset mask must be a vector with one value per Slot")
        reset_start = time.perf_counter()
        layout = self._layouts["reset_request"]
        encoded_levels = self._terrain_levels if terrain_levels is None else terrain_levels
        encode_start = time.perf_counter()
        encoded_reset_values = (
            self._sample_reset_values(reset_mask) if reset_values is None else reset_values
        )
        payload = self._encode_reset_request(layout, reset_mask, encoded_levels, encoded_reset_values)
        reset_encode_s = time.perf_counter() - encode_start
        _header, payload = self._session.reset(layout.layout_id, payload)
        decode_start = time.perf_counter()
        result_layout = self._layouts["reset_result"]
        mapped = BatchCodec.map_trusted(result_layout, payload)
        values, _state_valid, _fault_code, episode_index = self._decode_state_mapping(result_layout, mapped)
        try:
            response_mask = self._decode_reset_mask(mapped, result_layout)
        except SessionError:
            self.close("direct_session_reset_mask_invalid")
            raise
        request_mask = reset_mask.to(device=self._device, dtype=torch.bool)
        if not torch.equal(response_mask, request_mask):
            self.close("direct_session_reset_mask_mismatch")
            raise SessionError("Worker ResetResult mask does not match the request mask")
        self._episode_index = episode_index
        if self._terrain_enabled:
            self._terrain_levels = encoded_levels.to(device=self._device, dtype=torch.uint16).reshape(-1).clone()
        reset_decode_s = time.perf_counter() - decode_start
        self._last_reset_timing = {
            "client_reset_encode": reset_encode_s,
            **self._session.last_reset_timing,
            "client_reset_decode": reset_decode_s,
            "client_reset_roundtrip": time.perf_counter() - reset_start,
        }
        return PostResetState(
            values,
            response_mask,
            torch.zeros_like(episode_index, dtype=torch.uint16),
            episode_index,
        )

    def close(self, reason: str = "session_close") -> None:
        """Close the underlying Session once.

        Args:
            reason: Audit text passed to raw Session cleanup.

        Side effects:
            Mark the adapter closed and release raw Session resources. Repeated
            calls are no-ops; no retry or reconnect is attempted.
        """

        if self._closed:
            return
        self._closed = True
        self._session.close(reason)

    def _decode_state(
        self,
        layout: Layout,
        payload: bytes,
    ) -> tuple[dict[str, torch.Tensor], torch.Tensor, torch.Tensor, torch.Tensor]:
        """Decode named state fields and protocol system vectors."""

        mapped = BatchCodec.map_trusted(layout, payload)
        return self._decode_state_mapping(layout, mapped)

    def _decode_state_mapping(
        self,
        layout: Layout,
        mapped: Mapping[str, memoryview],
    ) -> tuple[dict[str, torch.Tensor], torch.Tensor, torch.Tensor, torch.Tensor]:
        """Decode one already boundary-checked payload mapping."""

        schema = cast(SessionSchema, self._schema)
        values = {
            str(descriptor["name"]): _tensor_from_segment(
                mapped[str(descriptor["name"])],
                layout.segment(str(descriptor["name"])),
                self._device,
            )
            for descriptor in schema.state_requirements
        }
        # Initialize and Reset frames carry a full valid/no-fault batch; only
        # StepResult supplies per-row validity and fault metadata from the Worker.
        state_valid = (
            _tensor_from_segment(
                mapped["system.state_valid"],
                layout.segment("system.state_valid"),
                self._device,
            ).bool()
            if layout.kind == "step_result"
            else torch.ones(self.num_slots, dtype=torch.bool, device=self._device)
        )
        fault_code = (
            _tensor_from_segment(
                mapped["system.slot_fault_code"],
                layout.segment("system.slot_fault_code"),
                self._device,
            )
            if layout.kind == "step_result"
            else torch.zeros(self.num_slots, dtype=torch.uint16, device=self._device)
        )
        # StepResult has no new episode index; retain the last Reset/Initialize
        # snapshot until the next Post-Reset response updates it.
        episode_index = (
            _tensor_from_segment(
                mapped["system.episode_index"],
                layout.segment("system.episode_index"),
                self._device,
            )
            if layout.kind in {"initial_state", "reset_result"}
            else self._episode_index.clone()
        )
        return values, state_valid, fault_code, episode_index

    def _validate_layout_contract(self) -> None:
        """Check Task-required segments against negotiated layouts once."""

        schema = cast(SessionSchema, self._schema)
        state_segments = {
            str(descriptor["name"]): (str(descriptor["dtype"]), _descriptor_shape(descriptor, self.num_slots))
            for descriptor in schema.state_requirements
        }
        action_segments = {
            str(descriptor["name"]): (str(descriptor["dtype"]), _descriptor_shape(descriptor, self.num_slots))
            for descriptor in schema.action_schema
        }
        for kind in ("initial_state", "step_result", "reset_result"):
            layout = self._layouts[kind]
            for name, (dtype, shape) in state_segments.items():
                _require_segment(layout, name, dtype, shape)
        for name, (dtype, shape) in action_segments.items():
            _require_segment(self._layouts["step_action"], name, dtype, shape)
        _require_segment(self._layouts["step_action"], "step_decimation", "int32", (1,))
        _require_segment(
            self._layouts["initial_state"],
            "system.episode_index",
            "uint64",
            (self.num_slots,),
        )
        if self._terrain_enabled:
            _require_segment(
                self._layouts["reset_request"],
                "terrain_level",
                "uint16",
                (self.num_slots,),
            )
        if self._robot_spec is not None and self._robot_spec.reset:
            _require_segment(
                self._layouts["reset_request"],
                "robot.reset.values",
                "float32",
                (self.num_slots, len(self._robot_spec.reset)),
            )
        _require_segment(self._layouts["step_result"], "system.state_valid", "uint8", (self.num_slots,))
        _require_segment(self._layouts["step_result"], "system.slot_fault_code", "uint16", (self.num_slots,))
        reset_shape = ((self.num_slots + 7) // 8,)
        _require_segment(self._layouts["reset_request"], "system.reset_mask", "uint8", reset_shape)
        _require_segment(self._layouts["reset_result"], "system.reset_mask", "uint8", reset_shape)
        _require_segment(self._layouts["reset_result"], "system.episode_index", "uint64", (self.num_slots,))

    def _decode_reset_mask(self, mapped: Mapping[str, memoryview], layout: Layout) -> torch.Tensor:
        """Decode the Worker-returned reset bitset for protocol comparison."""

        segment = layout.segment("system.reset_mask")
        raw = _tensor_from_segment(mapped["system.reset_mask"], segment, torch.device("cpu"))
        unused_bits = self.num_slots % 8
        if unused_bits and int(raw[-1]) & ~((1 << unused_bits) - 1):
            raise SessionError("Worker ResetResult mask sets unused Slot bits")
        bits = np.unpackbits(raw.numpy().astype(np.uint8, copy=False), bitorder="little")
        response_mask = torch.from_numpy(bits[: self.num_slots].astype(bool, copy=False))
        return response_mask.to(device=self._device)

    def _encode_commands(self, layout: Layout, command: PhysicalCommandBatch, step_decimation: int) -> bytes:
        """Encode each negotiated command segment in field-major layout order."""

        payload = bytearray(layout.payload_length)
        for segment in layout.segments:
            if segment.name == "step_decimation":
                raw_step = np.asarray([step_decimation], dtype=cast(Any, _NUMPY_DTYPES["int32"])).tobytes(order="C")
                payload[segment.offset : segment.offset + segment.byte_length] = raw_step
                continue
            # Convert through CPU little-endian NumPy storage because the wire
            # layout is field-major and device tensors are not wire buffers.
            values = command[segment.name].detach().to(device="cpu", dtype=torch.float32).contiguous().numpy()
            raw = (
                np.asarray(values, dtype=cast(Any, _NUMPY_DTYPES[segment.dtype]))
                .reshape(segment.shape)
                .tobytes(order="C")
            )
            payload[segment.offset : segment.offset + segment.byte_length] = raw
        return bytes(payload)

    def _encode_reset_request(
        self,
        layout: Layout,
        reset_mask: torch.Tensor,
        terrain_levels: torch.Tensor,
        reset_values: torch.Tensor | None,
    ) -> bytes:
        """Pack reset mask and the optional negotiated terrain-level segment."""

        payload = bytearray(self._encode_reset_mask(layout, reset_mask))
        terrain_segment = next((item for item in layout.segments if item.name == "terrain_level"), None)
        if terrain_segment is None:
            if self._terrain_enabled:
                raise SessionError("reset_request layout is missing terrain_level")
        else:
            if terrain_segment.dtype != "uint16" or terrain_segment.shape != (self.num_slots,):
                raise SessionError("reset_request terrain_level segment does not match the Slot batch")
            if terrain_levels.dtype == torch.bool or terrain_levels.is_floating_point() or terrain_levels.is_complex():
                raise SessionError("terrain levels must use an integer tensor")
            levels = terrain_levels.detach().to(device="cpu", dtype=torch.int64).reshape(-1)
            if levels.numel() != self.num_slots:
                raise SessionError("terrain levels must contain one value per Slot")
            if self._terrain_enabled:
                num_levels = cast(int, self._terrain_config["num_levels"])
                if bool(torch.any(levels < 0)) or bool(torch.any(levels >= num_levels)):
                    raise SessionError("terrain levels are outside the configured range")
            raw = np.asarray(levels.tolist(), dtype=cast(Any, _NUMPY_DTYPES["uint16"])).tobytes(order="C")
            payload[terrain_segment.offset : terrain_segment.offset + terrain_segment.byte_length] = raw
        reset_segment = next((item for item in layout.segments if item.name == "robot.reset.values"), None)
        if reset_segment is not None:
            if reset_values is None:
                raise SessionError("reset_request layout requires Robot reset values")
            values = reset_values.detach().to(device="cpu", dtype=torch.float32).reshape(-1)
            if values.numel() != math.prod(reset_segment.shape) or not bool(torch.isfinite(values).all()):
                raise SessionError("Robot reset values do not match the negotiated layout")
            raw_values = np.asarray(values.numpy(), dtype=cast(Any, _NUMPY_DTYPES["float32"])).tobytes(order="C")
            payload[reset_segment.offset : reset_segment.offset + reset_segment.byte_length] = raw_values
        elif reset_values is not None:
            raise SessionError("Robot reset values were supplied without a negotiated segment")
        return bytes(payload)

    def _sample_reset_values(self, reset_mask: torch.Tensor) -> torch.Tensor | None:
        """Sample Python-owned Robot reset targets into the fixed wire vector."""

        if self._robot_spec is None or not self._robot_spec.reset:
            return None
        semantics = self._session.config.worker.robot_semantics
        if semantics is None:
            raise SessionError("RobotSpec reset bindings have no RobotConfig owner")
        return sample_robot_reset_distributions(
            robot_spec=self._robot_spec,
            robot_semantics=semantics,
            run_seed=self._session.config.worker.run_seed,
            episode_index=self._episode_index,
            reset_mask=reset_mask,
        )

    @staticmethod
    def _encode_reset_mask(layout: Layout, reset_mask: torch.Tensor) -> bytes:
        """Pack a boolean Slot mask into the negotiated bitset segment."""

        payload = bytearray(layout.payload_length)
        segment = layout.segment("system.reset_mask")
        # Unused high bits in the final byte stay zero; the Worker rejects them
        # so a mask cannot address a Slot outside the negotiated batch.
        mask = reset_mask.detach().to(device="cpu", dtype=torch.bool).reshape(-1).numpy()
        packed = np.packbits(mask, bitorder="little")
        payload[segment.offset : segment.offset + segment.byte_length] = packed.tobytes()
        return bytes(payload)


def _trusted_layout(descriptor: Mapping[str, Any]) -> Layout:
    """Build a Layout from the validated Initialize response."""

    segments = tuple(
        Segment(
            str(item["name"]),
            str(item["dtype"]),
            tuple(cast(list[int], item["shape"])),
            int(item["offset"]),
            int(item["byte_length"]),
        )
        for item in cast(list[Mapping[str, Any]], descriptor["segments"])
    )
    return Layout(
        kind=str(descriptor["layout_kind"]),
        payload_length=int(descriptor["payload_length"]),
        layout_id=int(str(descriptor["layout_id"]), 16),
        layout_hash=str(descriptor["layout_hash"]),
        segments=segments,
    )


def _descriptor_shape(descriptor: Mapping[str, Any], num_slots: int) -> tuple[int, ...]:
    """Return the negotiated wire shape for one Task descriptor."""

    return (num_slots, *tuple(cast(list[int], descriptor["shape"])))


def _require_segment(layout: Layout, name: str, dtype: str, shape: tuple[int, ...]) -> Segment:
    """Require one protocol segment with the exact negotiated dtype and shape."""

    try:
        segment = layout.segment(name)
    except StopIteration as exc:
        raise SessionError(f"{layout.kind} layout is missing required segment {name}") from exc
    if segment.dtype != dtype or segment.shape != shape:
        raise SessionError(f"{layout.kind} segment {name} does not match the typed schema")
    return segment


def _tensor_from_segment(buffer: memoryview, segment: Segment, device: torch.device) -> torch.Tensor:
    """Copy one little-endian segment into the requested torch device."""

    dtype = _NUMPY_DTYPES[segment.dtype]
    values = np.frombuffer(
        buffer,
        dtype=cast(Any, dtype),
        count=math.prod(segment.shape),
    ).reshape(segment.shape).copy()
    return torch.from_numpy(values).to(device=device)


__all__ = ["UERLSessionAdapter"]

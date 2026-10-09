"""Coordinate framework-independent Direct-style environment orchestration."""

from __future__ import annotations

from collections.abc import Callable, Iterator, Mapping
from contextlib import contextmanager
from time import perf_counter
from typing import Any, TypeVar, cast

import torch

from ...errors import SessionError
from ..config import ResolvedRunConfig
from ..config.robot import RobotSpec
from ..mdp.lib.events import RecordingEventBridge
from ..mdp.managers.event import EventManager
from .curriculum import CurriculumManager, CurriculumStep
from .debug import TrainingDebugSink
from .profiling import StageProfiler
from .robot_observation import (
    ObservationShapeTable,
    validate_robot_observation_groups,
    validate_robot_observation_schema,
    validate_robot_shape_agreement,
)
from .session import DirectSession
from .task import DirectTask
from .types import (
    PhysicalCommandBatch,
    SessionSchema,
    StateBatch,
    StepContext,
    TerminationResult,
    TransitionState,
)

_StateBatchT = TypeVar("_StateBatchT", bound=StateBatch)
_DEFAULT_PHYSICS_DT = 1.0 / 60.0
_DECIMATION_STREAM_SALT = 0x5EEDDEC1A7E


class UERLDirectEnv:
    """Own one typed Session and expose Gymnasium-style five-tuple semantics.

    Keep Task mathematics on valid transition rows, capture terminal data before
    Reset, and merge only the selected post-reset rows into the next state.
    Initialization or Ready failure closes the Session before propagating the
    original exception.
    """

    @property
    def min_control_dt(self) -> float:
        """Shortest physical control window negotiated for this Session."""

        return self._physics_dt * self._decimation_range[0]

    @property
    def physics_dt(self) -> float:
        """Seconds per solver step; ``episode_solver_steps`` counts these."""

        return self._physics_dt

    def __init__(
        self,
        typed_session: DirectSession,
        task: DirectTask,
        resolved_config: ResolvedRunConfig | None = None,
        device: str | torch.device = "cpu",
        profiler: StageProfiler | None = None,
        curriculum_manager: CurriculumManager | None = None,
        event_manager: EventManager | None = None,
        event_manager_factory: Callable[[RobotSpec], EventManager] | None = None,
        initial_terrain_level: int | None = None,
        step_trace_callback: Callable[[Mapping[str, object]], None] | None = None,
        training_debug: TrainingDebugSink | None = None,
    ) -> None:
        """Create, initialize, ready, and fully reset one typed environment.

        Args:
            typed_session: A not-yet-initialized typed Session whose Slot count
                defines the environment batch.
            task: Task mathematics and schema provider for this environment.
            resolved_config: Optional immutable Run configuration retained for
                audit and seed-immutability checks.
            device: Torch device receiving copied state, observations, masks,
                rewards, and metrics.
            profiler: Optional per-stage latency profiler. When ``None`` no
                timing is recorded and ``step`` behaviour is unchanged.
            curriculum_manager: Optional curriculum terms. Scheduling goes through
                ``task.on_step`` / ``task.on_reset``; the environment only keeps a
                reference for terrain-level Session resets and checkpoints.
            event_manager: Optional event terms. The environment applies
                ``startup`` after Ready, ``reset`` before each Session reset, and
                ``interval`` after each completed transition; effects go through
                a Session bridge only.
            event_manager_factory: Optional factory called with the reflected
                ``RobotSpec`` after Initialize and Ready, for event terms whose
                preparation depends on UE topology.
            initial_terrain_level: Optional terrain level to use for the first
                reset of every Slot. This is used by deterministic playback;
                normal training leaves it unset so the curriculum starts at
                level zero.
            step_trace_callback: Optional observer for one-Slot deployment
                diagnostics. Each callback row keeps policy inputs, the
                pre-reset Worker transition, and any post-reset state distinct.
                ``None`` avoids all trace-row copies.
            training_debug: Optional bounded all-Slot observer. Records control
                windows, not intermediate Chaos solver substeps.

        The Worker-provided InitialState establishes schemas and canonical
        state after UE completes its post-spawn physics stabilization frame.
        DirectEnv then resets every Slot before exposing observations, so
        Python-owned Robot reset distributions define the first episode just
        as they define every later episode.

        Raises:
            Exception: Propagates the original initialization or Ready failure
                after closing the typed Session exactly once.
        """

        self.session = typed_session
        self.task = task
        self.resolved_config = resolved_config
        self.device = torch.device(device)
        task.align_batch(typed_session.num_slots)
        self._profiler = profiler
        self.num_envs = typed_session.num_slots
        if step_trace_callback is not None and self.num_envs != 1:
            raise ValueError("policy step tracing currently supports exactly one Slot")
        self._step_trace_callback = step_trace_callback
        self._training_debug = training_debug
        self._debug_control_step = 0
        worker_config = None if resolved_config is None else resolved_config.worker
        self._physics_dt = float(
            _DEFAULT_PHYSICS_DT if worker_config is None else worker_config.physics_dt
        )
        configured_decimation = (1, 1) if worker_config is None else worker_config.decimation
        self._decimation_range = (
            int(configured_decimation[0]),
            int(configured_decimation[1]),
        )
        stream_seed = (
            (0 if worker_config is None else int(worker_config.run_seed)) ^ _DECIMATION_STREAM_SALT
        ) & ((1 << 63) - 1)
        self._decimation_generator = torch.Generator(device="cpu")
        self._decimation_generator.manual_seed(stream_seed)
        self._action_interval_dt = torch.full(
            (self.num_envs,),
            self._physics_dt * self._decimation_range[0],
            dtype=torch.float32,
            device=self.device,
        )
        terrain_config = resolved_config.worker.terrain_config if resolved_config is not None else {}
        self._terrain_enabled = bool(terrain_config)
        self.curriculum_manager = curriculum_manager
        if curriculum_manager is not None:
            task.bind_curriculum_manager(curriculum_manager)
        if event_manager is not None and event_manager_factory is not None:
            raise SessionError("pass event_manager or event_manager_factory, not both")
        self.event_manager = event_manager
        self._event_manager_factory = event_manager_factory
        self._event_bridge = RecordingEventBridge()
        if event_manager is not None:
            event_manager.bind_bridge(self._event_bridge)
            task.bind_event_manager(event_manager)
        manager_levels = (
            None if curriculum_manager is None else curriculum_manager.terrain_levels()
        )
        if initial_terrain_level is not None:
            if not self._terrain_enabled:
                raise SessionError("initial terrain level requires a terrain configuration")
            num_levels = cast(int, terrain_config["num_levels"])
            if not 0 <= initial_terrain_level < num_levels:
                raise SessionError("initial terrain level is outside the configured range")
        if initial_terrain_level is not None:
            initial_levels: tuple[int, ...] = (initial_terrain_level,) * self.num_envs
        elif manager_levels is not None:
            initial_levels = tuple(int(level) for level in manager_levels.tolist())
        else:
            initial_levels = (0,) * self.num_envs
        self._terrain_levels = torch.tensor(
            initial_levels,
            dtype=torch.uint16,
            device=self.device,
        )
        self._episode_return = torch.zeros(self.num_envs, dtype=torch.float32, device=self.device)
        self.episode_length_buf = torch.zeros(self.num_envs, dtype=torch.long, device=self.device)
        self.episode_solver_steps = torch.zeros(self.num_envs, dtype=torch.int64, device=self.device)
        self._previous_policy_actions: torch.Tensor
        self._closed = False
        self._current_state: StateBatch
        self._last_observations: dict[str, torch.Tensor]

        try:
            bound_robot_spec: RobotSpec | None = None

            def bind_and_validate(
                robot_spec: RobotSpec,
                observation_shapes: ObservationShapeTable,
            ) -> SessionSchema:
                nonlocal bound_robot_spec
                bound_robot_spec = robot_spec
                validate_robot_shape_agreement(
                    robot_spec,
                    self.session.descriptor,
                    observation_shapes,
                )
                bound_schema = task.bind_robot_spec(
                    robot_spec,
                    observation_shapes=observation_shapes,
                )
                validate_robot_observation_schema(
                    robot_spec,
                    bound_schema.state_requirements,
                    observation_shapes,
                )
                validate_robot_observation_groups(
                    robot_spec,
                    task.observation_groups,
                    observation_shapes,
                )
                return bound_schema

            self.session.initialize_with_robot_spec(task.schema, bind_and_validate)
            self.session.acknowledge_ready()
            if self.event_manager is None and self._event_manager_factory is not None:
                if bound_robot_spec is None:
                    raise SessionError("event_manager_factory requires a bound RobotSpec")
                self.event_manager = self._event_manager_factory(bound_robot_spec)
                self.event_manager.bind_bridge(self._event_bridge)
                task.bind_event_manager(self.event_manager)
                self._event_manager_factory = None
            self._previous_policy_actions = torch.zeros(
                (self.num_envs, task.num_actions), dtype=torch.float32, device=self.device
            )
            if self.event_manager is not None:
                self.event_manager.apply("startup")
                staged_levels = self._event_bridge.terrain_levels
                if staged_levels is not None:
                    if not self._terrain_enabled:
                        raise SessionError("startup terrain events require a terrain configuration")
                    self._terrain_levels = staged_levels.to(device=self.device, dtype=torch.uint16)
                self._flush_event_effects()
            self.reset()
        except Exception:
            self._closed = True
            self.session.close("direct_env_initialize_failed")
            raise

    def reset(self, *, seed: int | None = None) -> tuple[Mapping[str, torch.Tensor], dict[str, Any]]:
        """Reset every Slot and rebuild observations.

        Args:
            seed: Accepted for Gymnasium compatibility. It is intentionally not
                applied to an active Run; create a new Session to change the Run
                seed.

        Returns:
            A pair containing cloned post-reset observation groups and an info
            mapping with the full-batch ``episode_index`` tensor.

        Raises:
            SessionError: If the environment was already closed.
            Exception: Propagates a typed Session reset failure after the Session
                applies its own failure cleanup.

        Side effects:
            Reset all Slots through the same path used during construction and
            later episodes, zero the Python episode-length buffer, replace the
            current state with Post-Reset State, and update cached observations.

        """

        self._ensure_open()
        del seed
        reset_mask = torch.ones(self.num_envs, dtype=torch.bool, device=self.device)
        post_reset = self._move_state(self._reset_slots(reset_mask))
        if self.event_manager is not None:
            self.event_manager.reset(reset_mask)
        self.task.on_reset(reset_mask, post_reset.values)
        self.episode_length_buf.zero_()
        self.episode_solver_steps.zero_()
        self._episode_return.zero_()
        self._previous_policy_actions.zero_()
        self._action_interval_dt.fill_(self._physics_dt * self._decimation_range[0])
        self._current_state = post_reset
        self._last_observations = _clone_observations(
            self.task.build_observations(
                self._task_values(post_reset.values),
                post_reset.state_valid,
                self._previous_policy_actions,
                self._action_interval_dt,
            )
        )
        info: dict[str, Any] = {"episode_index": post_reset.episode_index.clone()}
        if self._terrain_enabled:
            info["terrain_level"] = self._terrain_levels.clone()
        return _clone_observations(self._last_observations), info

    def get_observations(self) -> Mapping[str, torch.Tensor]:
        """Return a cloned view of the current post-reset observation groups.

        Returns:
            The latest full-batch observations cached by the Direct environment.
            The returned tensors are copies and can be moved or consumed by a
            training adapter without mutating the environment cache.

        Raises:
            SessionError: If the environment was already closed.
        """

        self._ensure_open()
        return _clone_observations(self._last_observations)

    def step(
        self,
        policy_actions: torch.Tensor,
    ) -> tuple[Mapping[str, torch.Tensor], torch.Tensor, torch.Tensor, torch.Tensor, dict[str, Any]]:
        """Execute one fixed transition, Reset, and observation sequence.

        Args:
            policy_actions: Batch-first policy actions in stable Slot order. Its
                first dimension must equal ``num_envs``; the Task interprets the
                remaining dimensions according to its action schema.

        Returns:
            A five-tuple of next observations, rewards, terminated mask,
            truncated mask, and an info mapping. ``info`` contains
            ``terminal_observation``, ``terminal_raw_state``,
            ``terminal_observation_valid``, ``termination_reason``,
            ``slot_fault_code``, ``episode_index``, and ``episode_metrics``.

        Raises:
            SessionError: If the environment is closed or the Session rejects
                the lifecycle operation.
            Exception: Propagates Task or Session failures according to the
                underlying component's failure contract.

        Side effects:
            Advance the transition, update episode lengths, reset rows selected
            by ``terminated OR truncated``, merge only their Post-Reset values,
            and replace the cached current state and observations.
        """

        self._ensure_open()
        policy_actions = policy_actions.to(device=self.device)
        step_decimation = self._sample_step_decimation()
        transition_dt = self._physics_dt * step_decimation
        previous_state = self._current_state
        debug = self._training_debug
        if debug is not None and not debug.captures(self._debug_control_step):
            debug = None
        if self._training_debug is not None:
            self.task.capture_reward_terms(debug is not None)
        if debug is not None:
            debug.record("environment_input", self._debug_control_step, {
                "slot_ids": torch.arange(self.num_envs),
                "episode_index": previous_state.episode_index,
                "episode_steps": self.episode_length_buf,
                "episode_solver_steps": self.episode_solver_steps,
                "observation_dt": self._action_interval_dt,
                "raw_state": previous_state.values,
                "state_valid": previous_state.state_valid,
                "fault_code": previous_state.slot_fault_code,
                "observation_groups": self._last_observations,
                "commands": self.task.command_source.current(),
                "previous_policy_actions": self._previous_policy_actions,
                "policy_actions": policy_actions,
            })
        trace_input: dict[str, object] | None = None
        input_episode_step = 0
        input_episode_elapsed_s = 0.0
        input_observation_dt_s = 0.0
        if self._step_trace_callback is not None:
            input_episode_step = int(self.episode_length_buf[0].item())
            input_episode_elapsed_s = float(self.episode_solver_steps[0].item()) * self._physics_dt
            input_observation_dt_s = float(self._action_interval_dt[0].item())
            trace_input = {
                "raw_state": _trace_tensor_mapping(previous_state.values, 0),
                "state_valid": bool(previous_state.state_valid[0].item()),
                "fault_code": int(previous_state.slot_fault_code[0].item()),
                "observation_groups": _trace_tensor_mapping(self._last_observations, 0),
                "previous_action": _trace_tensor_row(self._previous_policy_actions, 0),
                "commands": {},
            }
        previous_task_state = StateBatch(
            self._task_values(previous_state.values),
            previous_state.state_valid,
            previous_state.slot_fault_code,
            previous_state.episode_index,
        )
        with self._stage("preprocess"):
            physical_command = self.task.preprocess_actions(policy_actions, previous_task_state.values)
        published_velocity = self.task.published_velocity()
        command_velocity = None if published_velocity is None else published_velocity.clone()
        if trace_input is not None:
            trace_input["commands"] = _trace_tensor_mapping(self.task.command_source.current(), 0)
        with self._stage("step_roundtrip"):
            transition = self.session.step(physical_command, step_decimation=step_decimation)
            move_start = perf_counter()
            transition = self._move_state(transition)
            state_move_s = perf_counter() - move_start
            if self._profiler is not None:
                self._profiler.record_step_details({
                    **self.session.last_step_timing,
                    "client_state_move": state_move_s,
                })
        self.episode_length_buf += 1
        self.episode_solver_steps += step_decimation
        episode_elapsed_s = self.episode_solver_steps.to(dtype=torch.float64) * self._physics_dt
        transition_task_state = StateBatch(
            self._task_values(transition.values),
            transition.state_valid,
            transition.slot_fault_code,
            transition.episode_index,
        )
        scored_values = dict(transition_task_state.values)
        if command_velocity is not None:
            scored_values["velocity"] = command_velocity
        scored_state = StateBatch(
            scored_values,
            transition_task_state.state_valid,
            transition_task_state.slot_fault_code,
            transition_task_state.episode_index,
        )

        # Faulted or invalid Worker rows receive fixed handling; compact only
        # valid rows so task mathematics cannot consume fallback state values.
        with self._stage("reward_term"):
            task_valid = transition.state_valid & transition.slot_fault_code.eq(0)
            valid_ids = torch.nonzero(task_valid, as_tuple=False).flatten()
            valid_context: StepContext | None = None
            valid_terminations: TerminationResult | None = None
            terminated = torch.zeros(self.num_envs, dtype=torch.bool, device=self.device)
            truncated = torch.zeros(self.num_envs, dtype=torch.bool, device=self.device)
            terminated[~task_valid] = True
            rewards = torch.full(
                (self.num_envs,),
                self.task.slot_fault_reward,
                dtype=torch.float32,
                device=self.device,
            )
            metrics: Mapping[str, torch.Tensor | float] = {}

            if valid_ids.numel():
                valid_context = _compact_context(
                    previous_task_state,
                    scored_state,
                    physical_command,
                    self._previous_policy_actions,
                    policy_actions,
                    transition_dt,
                    self.episode_length_buf,
                    episode_elapsed_s,
                    valid_ids,
                )
                valid_terminations = self.task.compute_terminations(valid_context)
                terminated[valid_ids] = valid_terminations.terminated.to(device=self.device, dtype=torch.bool)
                truncated[valid_ids] = valid_terminations.truncated.to(device=self.device, dtype=torch.bool)
                valid_rewards = self.task.compute_rewards(valid_context, valid_terminations)
                rewards[valid_ids] = valid_rewards.to(device=self.device, dtype=torch.float32)
                metrics = _expand_mapping(
                    self.task.collect_metrics(valid_context, valid_terminations),
                    valid_ids,
                    self.num_envs,
                    self.device,
                )

        self._episode_return += rewards
        if debug is not None:
            debug.record("environment_transition", self._debug_control_step, {
                "slot_ids": torch.arange(self.num_envs),
                "valid_slot_ids": valid_ids,
                "slot_fault_reward": self.task.slot_fault_reward,
                "physics_dt": self._physics_dt,
                "step_decimation": step_decimation,
                "transition_dt": transition_dt,
                "physical_commands": physical_command.values,
                "scored_command_velocity": command_velocity,
                "previous_task_state": previous_task_state.values,
                "reward_input_state": scored_state.values,
                "raw_state": transition.values,
                "state_valid": transition.state_valid,
                "fault_code": transition.slot_fault_code,
                "episode_index": transition.episode_index,
                "episode_steps": self.episode_length_buf,
                "episode_solver_steps": self.episode_solver_steps,
                "weighted_reward_terms_compact": self.task.debug_reward_terms(),
                "rewards": rewards,
                "terminated": terminated,
                "truncated": truncated,
            })

        # Terminal observation backfill stays on the env (ticket 29): invalid /
        # faulted rows are Slot-lifecycle fallbacks, not observation semantics.
        # Copy the previous valid observation so terminal_obs stays finite without
        # feeding Worker fault payloads into Task mathematics.
        with self._stage("terminal_obs"):
            terminal_raw_state = _clone_mapping(transition.values)
            invalid = ~task_valid
            terminal_observation = _clone_observations(
                self.task.build_observations(
                    scored_state.values,
                    task_valid,
                    policy_actions,
                    torch.full(
                        (self.num_envs,),
                        transition_dt,
                        dtype=torch.float32,
                        device=self.device,
                    ),
                )
            )
            for name, value in terminal_observation.items():
                value[invalid] = self._last_observations[name][invalid]

        # Keep terminal snapshots from TransitionState; only rows selected by
        # the combined mask may be replaced with Post-Reset State.
        reset_mask = terminated | truncated
        post_reset: StateBatch | None = None
        if bool(reset_mask.any()):
            metrics = {**metrics, **self.task.episode_reward_log(reset_mask)}
        terminal_episode_length = self.episode_length_buf.clone()
        next_values = _clone_mapping(transition.values)
        next_state_valid = transition.state_valid.clone()
        next_fault_code = transition.slot_fault_code.clone()
        next_episode_index = transition.episode_index.clone()
        next_policy_actions = policy_actions.clone()
        curriculum_metrics = self.task.on_step(
            CurriculumStep(
                transition_state=transition.values,
                metrics=metrics,
                state_valid=task_valid,
                terminated=terminated,
                truncated=truncated,
                episode_lengths=terminal_episode_length,
                transition_dt=transition_dt,
                command_velocity=_planar_command(command_velocity, self.num_envs, self.device),
            )
        )
        metrics = {**metrics, **curriculum_metrics}
        manager_levels = (
            None if self.curriculum_manager is None else self.curriculum_manager.terrain_levels()
        )
        if manager_levels is not None:
            self._terrain_levels = manager_levels.to(device=self.device, dtype=torch.uint16)
        next_action_interval_dt = torch.full(
            (self.num_envs,), transition_dt, dtype=torch.float32, device=self.device
        )
        if bool(reset_mask.any()):
            with self._stage("reset_roundtrip"):
                post_reset_state = self._move_state(self._reset_slots(reset_mask))
            post_reset = post_reset_state
            if self.event_manager is not None:
                self.event_manager.reset(reset_mask)
            self.task.on_reset(reset_mask, post_reset_state.values)
            for name, value in next_values.items():
                value[reset_mask] = post_reset_state.values[name][reset_mask]
            next_state_valid[reset_mask] = post_reset_state.state_valid[reset_mask]
            next_fault_code = _merge_metadata_rows(
                next_fault_code, post_reset_state.slot_fault_code, reset_mask
            )
            next_episode_index = _merge_metadata_rows(
                next_episode_index, post_reset_state.episode_index, reset_mask
            )
            self.episode_length_buf[reset_mask] = 0
            self.episode_solver_steps[reset_mask] = 0
            self._episode_return[reset_mask] = 0.0
            next_policy_actions[reset_mask] = 0.0
            next_action_interval_dt[reset_mask] = self._physics_dt * self._decimation_range[0]

        self.task.refresh_command(next_values, dt=transition_dt)
        if self.event_manager is not None:
            self.event_manager.apply("interval", dt=transition_dt)
            self._flush_event_effects()

        self._current_state = TransitionState(
            next_values,
            next_state_valid,
            next_fault_code,
            next_episode_index,
        )
        self._action_interval_dt = next_action_interval_dt
        self._previous_policy_actions = next_policy_actions
        with self._stage("build_obs"):
            observations = _clone_observations(
                self.task.build_observations(
                    self._task_values(next_values),
                    next_state_valid,
                    self._previous_policy_actions,
                    self._action_interval_dt,
                )
            )
        self._last_observations = _clone_observations(observations)
        reason: Mapping[str, Any] = {}
        if valid_terminations is not None:
            reason = _expand_mapping(valid_terminations.reason, valid_ids, self.num_envs, self.device)
        if self._profiler is not None:
            metrics = {**metrics, **self._profiler.commit_step()}
        info: dict[str, Any] = {
            "terminal_observation": terminal_observation,
            "terminal_raw_state": terminal_raw_state,
            "terminal_observation_valid": task_valid.clone(),
            "termination_reason": reason,
            "slot_fault_code": transition.slot_fault_code.clone(),
            "episode_index": next_episode_index.clone(),
            "terminal_episode_length": terminal_episode_length,
            "episode_metrics": metrics,
            "transition_dt": transition_dt,
            "action_interval_dt": self._action_interval_dt.clone(),
        }
        if self._terrain_enabled:
            info["terrain_level"] = self._terrain_levels.clone()
        if self._step_trace_callback is not None and trace_input is not None:
            self._step_trace_callback(
                {
                    "kind": "step",
                    "episode_index": int(previous_state.episode_index[0].item()),
                    "episode_step": input_episode_step,
                    "phase": "post_reset_input" if input_episode_step == 0 else "post_window_input",
                    "clocks": {
                        "physics_dt_s": self._physics_dt,
                        "input_observation_dt_s": input_observation_dt_s,
                        "step_decimation": step_decimation,
                        "transition_dt_s": transition_dt,
                        "input_episode_elapsed_s": input_episode_elapsed_s,
                        "transition_episode_elapsed_s": float(episode_elapsed_s[0].item()),
                    },
                    "input": trace_input,
                    "action": {
                        "policy_action": _trace_tensor_row(policy_actions, 0),
                        "physical_commands": _trace_tensor_mapping(physical_command.values, 0),
                    },
                    "transition": {
                        "raw_state": _trace_tensor_mapping(transition.values, 0),
                        "state_valid": bool(transition.state_valid[0].item()),
                        "fault_code": int(transition.slot_fault_code[0].item()),
                        "episode_index": int(transition.episode_index[0].item()),
                        "observation_groups": _trace_tensor_mapping(terminal_observation, 0),
                        "reward": float(rewards[0].item()),
                        "terminated": bool(terminated[0].item()),
                        "truncated": bool(truncated[0].item()),
                        "termination_reason": _trace_row_value(reason, 0),
                        "terminal_episode_step": int(terminal_episode_length[0].item()),
                    },
                    "reset": (
                        None
                        if post_reset is None
                        else {
                            "raw_state": _trace_tensor_mapping(next_values, 0),
                            "state_valid": bool(next_state_valid[0].item()),
                            "fault_code": int(next_fault_code[0].item()),
                            "episode_index": int(next_episode_index[0].item()),
                            "observation_groups": _trace_tensor_mapping(observations, 0),
                        }
                    ),
                }
            )
        if debug is not None:
            debug.record("environment_output", self._debug_control_step, {
                "slot_ids": torch.arange(self.num_envs),
                "reset_mask": reset_mask,
                "raw_state": next_values,
                "state_valid": next_state_valid,
                "fault_code": next_fault_code,
                "episode_index": next_episode_index,
                "episode_steps": self.episode_length_buf,
                "episode_solver_steps": self.episode_solver_steps,
                "commands": self.task.command_source.current(),
                "observation_groups": observations,
                "previous_policy_actions": self._previous_policy_actions,
                "terminal_observation": terminal_observation,
                "termination_reason": reason,
            })
        if self._training_debug is not None:
            self._debug_control_step += 1
        return observations, rewards, terminated, truncated, info

    def close(self, reason: str = "direct_env_close") -> None:
        """Close the typed Session exactly once without retrying or reconnecting.

        Args:
            reason: Audit text passed to the typed Session when ownership cleanup
                is performed.

        Side effects:
            Mark the environment closed and release Session-owned resources.
            Repeated calls are no-ops, including repeated calls with another
            reason.
        """

        if self._closed:
            return
        self._closed = True
        self.session.close(reason)

    def curriculum_state_dict(self) -> dict[str, dict[str, object]]:
        """Return checkpoint state for the optional curriculum manager."""

        if self.curriculum_manager is None:
            return {}
        return self.curriculum_manager.state_dict()

    def load_curriculum_state_dict(self, state: Mapping[str, object]) -> None:
        """Restore curriculum state and start fresh episodes at its saved stage."""

        if self.curriculum_manager is None:
            if state:
                raise ValueError("checkpoint contains curriculum state but this Task has no curriculum")
            return
        self.curriculum_manager.load_state_dict(state)
        manager_levels = self.curriculum_manager.terrain_levels()
        if manager_levels is not None:
            self._terrain_levels = manager_levels.to(device=self.device, dtype=torch.uint16)
        self.reset()

    def random_state_dict(self) -> dict[str, object]:
        """Return the dedicated per-step decimation sampler state."""

        return {"step_decimation_generator": self._decimation_generator.get_state().clone()}

    def load_random_state_dict(self, state: Mapping[str, object]) -> None:
        """Restore the dedicated per-step decimation sampler state."""

        if set(state) != {"step_decimation_generator"}:
            raise ValueError("DirectEnv random state fields are invalid")
        generator_state = state["step_decimation_generator"]
        if not isinstance(generator_state, torch.Tensor) or generator_state.dtype != torch.uint8:
            raise ValueError("DirectEnv decimation generator state must be a uint8 tensor")
        self._decimation_generator.set_state(generator_state.detach().cpu())

    def _sample_step_decimation(self) -> int:
        """Sample one inclusive decimation range value shared by all Slots."""

        minimum, maximum = self._decimation_range
        if minimum == maximum:
            return minimum
        return int(
            torch.randint(
                minimum,
                maximum + 1,
                (),
                generator=self._decimation_generator,
                device="cpu",
            ).item()
        )

    def _reset_slots(self, reset_mask: torch.Tensor) -> StateBatch:
        """Reset through the terrain-aware Session surface when configured."""

        if self.event_manager is not None:
            self.event_manager.apply("reset", mask=reset_mask)
            staged_levels = self._event_bridge.terrain_levels
            if staged_levels is not None and self._terrain_enabled:
                merged = self._terrain_levels.to(dtype=torch.int64)
                merged[reset_mask] = staged_levels.to(dtype=torch.int64)[reset_mask]
                self._terrain_levels = merged.to(dtype=torch.uint16)
                self._event_bridge.terrain_levels = None
            staged_reset_values = self._event_bridge.robot_reset_values
            self._event_bridge.robot_reset_values = None
            self._flush_event_effects()
        else:
            staged_reset_values = None
        if self._terrain_enabled:
            if staged_reset_values is None:
                return self.session.reset(reset_mask, self._terrain_levels)
            return self.session.reset(reset_mask, self._terrain_levels, reset_values=staged_reset_values)
        if staged_reset_values is None:
            return self.session.reset(reset_mask)
        return self.session.reset(reset_mask, reset_values=staged_reset_values)

    def _flush_event_effects(self) -> None:
        """Send queued EventManager effects through the typed Session seam."""

        for effect in self._event_bridge.drain_effects():
            response = self.session.apply_event(effect)
            if effect.kind == "terrain_tier_params":
                self._apply_terrain_event_levels(response)

    def _apply_terrain_event_levels(self, response: Mapping[str, Any]) -> None:
        """Merge Worker-selected pre-generated terrain tiers into local state."""

        levels = response.get("terrain_levels")
        if not isinstance(levels, list):
            return
        merged = self._terrain_levels.to(dtype=torch.int64)
        for item in levels:
            if not isinstance(item, Mapping):
                raise SessionError("terrain event response contains an invalid level row")
            slot_id = item.get("slot_id")
            level = item.get("level")
            if (
                not isinstance(slot_id, int)
                or isinstance(slot_id, bool)
                or not isinstance(level, int)
                or isinstance(level, bool)
                or not 0 <= slot_id < self.num_envs
            ):
                raise SessionError("terrain event response contains an invalid Slot level")
            merged[slot_id] = level
        self._terrain_levels = merged.to(dtype=torch.uint16)

    def _task_values(self, values: Mapping[str, torch.Tensor]) -> Mapping[str, torch.Tensor]:
        if self.curriculum_manager is None:
            return values
        return self.curriculum_manager.transform_state(values)

    def _ensure_open(self) -> None:
        if self._closed:
            raise SessionError("Direct environment is closed")

    @contextmanager
    def _stage(self, name: str) -> Iterator[None]:
        """Single disableable profiler seam for step stages (ticket 29).

        When no profiler is attached this is a no-op; callers keep the same
        ``with self._stage(...)`` shape either way.
        """

        if self._profiler is None:
            yield
            return
        with self._profiler.stage(name):
            yield

    def _move_state(self, state: _StateBatchT) -> _StateBatchT:
        """Move a typed state batch to the environment device."""

        values = {name: value.to(device=self.device).clone() for name, value in state.values.items()}
        state_valid = state.state_valid.to(device=self.device, dtype=torch.bool).clone()
        fault_code = state.slot_fault_code.to(device=self.device).clone()
        episode_index = state.episode_index.to(device=self.device).clone()
        return type(state)(values, state_valid, fault_code, episode_index)


def _compact_context(
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
        raw_state=_select_mapping(previous_state.values, valid_ids),
        previous_policy_actions=previous_policy_actions.index_select(0, valid_ids),
        policy_actions=policy_actions.index_select(0, valid_ids),
        physical_command=PhysicalCommandBatch(_select_mapping(physical_command.values, valid_ids)),
        transition_state=_select_mapping(transition.values, valid_ids),
        state_valid=transition.state_valid[valid_ids],
        slot_fault_code=transition.slot_fault_code[valid_ids],
        episode_steps=episode_steps[valid_ids],
        episode_elapsed_s=episode_elapsed_s[valid_ids],
        transition_dt=transition_dt,
        slot_ids=valid_ids,
    )


def _planar_command(
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


def _merge_metadata_rows(
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


def _select_mapping(values: Mapping[str, torch.Tensor], indexes: torch.Tensor) -> dict[str, torch.Tensor]:
    """Select the same Slot rows from each named Tensor."""

    return {name: value[indexes] for name, value in values.items()}


def _clone_mapping(values: Mapping[str, torch.Tensor]) -> dict[str, torch.Tensor]:
    """Copy a named batch before sparse reset merge or terminal capture."""

    return {name: value.clone() for name, value in values.items()}


def _clone_observations(values: Mapping[str, torch.Tensor]) -> dict[str, torch.Tensor]:
    """Copy Task observation groups so terminal data cannot alias live state."""

    return {name: value.clone() for name, value in values.items()}


def _trace_tensor_row(value: torch.Tensor, slot_index: int) -> list[bool | float | int]:
    """Copy one batch row into YAML-safe scalars only when tracing is enabled."""

    return cast(list[bool | float | int], value[slot_index].detach().cpu().reshape(-1).tolist())


def _trace_tensor_mapping(
    values: Mapping[str, torch.Tensor],
    slot_index: int,
) -> dict[str, list[bool | float | int]]:
    """Copy named tensor fields for one traced Slot."""

    return {name: _trace_tensor_row(value, slot_index) for name, value in values.items()}


def _trace_row_value(value: object, slot_index: int) -> object:
    """Select one Slot from termination details and convert tensors to YAML values."""

    if isinstance(value, torch.Tensor):
        if value.ndim == 0:
            return value.item()
        return value[slot_index].detach().cpu().reshape(-1).tolist()
    if isinstance(value, Mapping):
        return {str(name): _trace_row_value(item, slot_index) for name, item in value.items()}
    if isinstance(value, (tuple, list)):
        return [_trace_row_value(item, slot_index) for item in value]
    return value


def _expand_mapping(
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


__all__ = ["UERLDirectEnv"]

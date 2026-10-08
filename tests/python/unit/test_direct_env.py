"""Verify the fixed DirectEnv lifecycle against a typed in-memory Session."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import replace
from typing import Any, cast

import pytest
import torch

from uerl import (
    CurriculumManager,
    CurriculumStep,
    CurriculumTerm,
    DirectTask,
    DirectTaskConfig,
    InitialState,
    LaunchMode,
    LoggingConfig,
    ObservationShapeTable,
    PhysicalCommandBatch,
    PostResetState,
    ResolvedRunConfig,
    RobotSpec,
    RslRlRunnerConfig,
    SessionConfig,
    SessionSchema,
    StepContext,
    TerminationResult,
    TransitionState,
    UERLDirectEnv,
    WorkerConfig,
)
from uerl.core.mdp.lib.curriculum import TerrainLevelTerm
from uerl.core.mdp.lib.events import EventEffect
from uerl.core.mdp.lib.terminations import time_out_seconds
from uerl.training.rsl_rl import UERLVecEnvWrapper


class _LifecycleTask(DirectTask):
    """Expose terminal and timeout ordering through a small task state."""

    def __init__(
        self,
        *,
        max_episode_steps: int = 3,
        slot_fault_reward: float = -7.0,
    ) -> None:
        super().__init__(DirectTaskConfig(max_episode_steps=max_episode_steps, slot_fault_reward=slot_fault_reward))
        self.valid_context_rows: list[int] = []
        self.observed_previous_actions: list[torch.Tensor] = []
        self.reward_previous_actions: list[torch.Tensor] = []
        self.reward_transition_values: list[torch.Tensor] = []
        self.context_transition_dts: list[float | None] = []
        self.observation_control_frame_dts: list[torch.Tensor | None] = []

    @property
    def schema(self) -> SessionSchema:
        """Return one scalar state and command field for lifecycle assertions."""

        return SessionSchema(
            (
                {
                    "name": "state.value",
                    "dtype": "float32",
                    "shape": [],
                    "unit": "m",
                    "frame": "slot/world",
                    "semantic": "position",
                    "source": "test.robot",
                },
            ),
            (
                {
                    "name": "cmd.force",
                    "dtype": "float32",
                    "shape": [],
                    "unit": "N",
                    "frame": "slot/world",
                    "semantic": "force",
                    "source": "test.robot",
                },
            ),
        )

    @property
    def observation_groups(self) -> Mapping[str, tuple[str, ...]]:
        """Expose the scalar state as the policy observation group."""

        return {"policy": ("state.value",)}

    @property
    def num_actions(self) -> int:
        """Expose the fake command width used by the lifecycle assertions."""

        return 1

    def preprocess_actions(
        self,
        policy_actions: torch.Tensor,
        raw_state: Mapping[str, torch.Tensor],
    ) -> PhysicalCommandBatch:
        """Preserve the batch shape for the fake Worker."""

        return PhysicalCommandBatch({"cmd.force": policy_actions + raw_state["state.value"] * 0})

    def build_observations(
        self,
        raw_state: Mapping[str, torch.Tensor],
        state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor,
        control_frame_dt: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        """Clone the scalar state group."""

        del state_valid
        self.observed_previous_actions.append(previous_policy_actions.clone())
        self.observation_control_frame_dts.append(
            None if control_frame_dt is None else control_frame_dt.clone()
        )
        return {"policy": raw_state["state.value"].clone()}

    def compute_terminations(self, context: StepContext) -> TerminationResult:
        """Terminate values above one and truncate valid rows at the configured limit."""

        self.valid_context_rows.append(int(context.transition_state["state.value"].shape[0]))
        self.context_transition_dts.append(context.transition_dt)
        value = context.transition_state["state.value"].flatten()
        terminated = value >= 1.0
        truncated = (context.episode_steps >= self.max_episode_steps) & ~terminated
        return TerminationResult(terminated, truncated, {"threshold": value})

    def compute_rewards(self, context: StepContext, terminations: TerminationResult) -> torch.Tensor:
        """Return the test reward from pre-reset values and termination masks."""

        self.reward_previous_actions.append(context.previous_policy_actions.clone())
        value = context.transition_state["state.value"].flatten()
        self.reward_transition_values.append(value.clone())
        return torch.where(terminations.terminated, torch.full_like(value, -2.0), torch.ones_like(value))

    def collect_metrics(
        self,
        context: StepContext,
        terminations: TerminationResult,
    ) -> Mapping[str, torch.Tensor | float]:
        """Return one metric per compact valid row."""

        return {"valid_rows": torch.ones(context.episode_steps.shape[0], device=context.episode_steps.device)}


class _FakeDirectSession:
    """Record lifecycle events and sparse reset masks through a typed Session fake."""

    num_slots = 2

    def __init__(
        self,
        transition: TransitionState,
        post_reset: PostResetState,
        initial_post_reset: PostResetState | None = None,
    ) -> None:
        self.events: list[str] = []
        self.transition = transition
        self.post_reset = post_reset
        self.initial_post_reset = initial_post_reset
        self.reset_masks: list[torch.Tensor] = []
        self.reset_terrain_levels: list[torch.Tensor | None] = []
        self.step_decimations: list[int] = []
        self.applied_events: list[EventEffect] = []
        self.close_calls = 0
        self.descriptor: Mapping[str, Any] = {}
        self.num_slots = int(transition.state_valid.numel())
        self.initial = InitialState(
            {"state.value": torch.zeros_like(transition["state.value"])},
            torch.ones(self.num_slots, dtype=torch.bool),
            torch.zeros(self.num_slots, dtype=torch.uint16),
            torch.zeros(self.num_slots, dtype=torch.uint64),
        )

    @property
    def last_step_timing(self) -> Mapping[str, float]:
        return {}

    @property
    def last_reset_timing(self) -> Mapping[str, float]:
        return {}

    def _record(self, event: str) -> None:
        self.events.append(event)

    def initialize_with_robot_spec(
        self,
        schema: SessionSchema,
        bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
    ) -> InitialState:
        """Record the required two-phase Initialize contract."""

        del schema, bind_robot_spec
        self._record("initialize")
        return self.initial

    def acknowledge_ready(self) -> None:
        """Record the Ready transition required before Step."""

        self._record("ready")

    def step(self, command: PhysicalCommandBatch, step_decimation: int | None = None) -> TransitionState:
        """Record Step, validate command shape, and return the scripted Transition."""

        assert step_decimation is not None
        self.step_decimations.append(step_decimation)
        self._record("step")
        assert command["cmd.force"].shape == (self.num_slots, 1)
        return self.transition

    def reset(
        self,
        reset_mask: torch.Tensor,
        terrain_levels: torch.Tensor | None = None,
        reset_values: torch.Tensor | None = None,
    ) -> PostResetState:
        """Record the sparse mask and return the scripted Post-Reset State."""

        del reset_values
        self._record("reset")
        self.reset_masks.append(reset_mask.clone())
        self.reset_terrain_levels.append(
            terrain_levels.clone() if terrain_levels is not None else None
        )
        if len(self.reset_masks) == 1:
            if self.initial_post_reset is not None:
                return self.initial_post_reset
            return PostResetState(
                self.initial.values,
                self.initial.state_valid,
                self.initial.slot_fault_code,
                self.initial.episode_index,
            )
        return self.post_reset

    def apply_event(self, effect: EventEffect) -> Mapping[str, Any]:
        """Record an event reaching the typed Session seam."""

        self.applied_events.append(effect)
        self.events.append(f"event:{effect.kind}")
        return {"applied": True, "kind": effect.kind}

    def close(self, reason: str = "session_close") -> None:
        """Count close calls so initialization and idempotency are observable."""

        self.close_calls += 1
        self.events.append("close")


class _OffsetCurriculum(CurriculumTerm):
    """Expose generic state transformation and reset ordering to DirectEnv."""

    def __init__(self) -> None:
        self.steps: list[CurriculumStep] = []
        self.resets: list[torch.Tensor] = []

    def transform_state(self, values: Mapping[str, torch.Tensor]) -> Mapping[str, torch.Tensor]:
        return {**values, "state.value": values["state.value"] + 0.5}

    def update(self, step: CurriculumStep) -> Mapping[str, torch.Tensor | float]:
        self.steps.append(step)
        return {"completed": (step.terminated | step.truncated).float().sum()}

    def reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_state: Mapping[str, torch.Tensor],
    ) -> None:
        del post_reset_state
        self.resets.append(reset_mask.clone())


class _OverlappingTerminationTask(_LifecycleTask):
    """Expose a fall on the same frame as the episode horizon."""

    def compute_terminations(self, context: StepContext) -> TerminationResult:
        result = super().compute_terminations(context)
        return TerminationResult(
            result.terminated,
            context.episode_steps >= self.max_episode_steps,
            result.reason,
        )


def _state(value: list[float], *, valid: list[bool] | None = None, faults: list[int] | None = None) -> TransitionState:
    slot_count = len(value)
    return TransitionState(
        {"state.value": torch.tensor(value, dtype=torch.float32).reshape(slot_count, 1)},
        torch.tensor(valid if valid is not None else [True] * slot_count),
        torch.tensor(faults if faults is not None else [0] * slot_count, dtype=torch.uint16),
        torch.zeros(slot_count, dtype=torch.uint64),
    )


def _post_reset(value: list[float], *, episode_index: list[int] | None = None) -> PostResetState:
    slot_count = len(value)
    if episode_index is None:
        episode_index = [1, 0] if slot_count == 2 else [0] * slot_count
    return PostResetState(
        {"state.value": torch.tensor(value, dtype=torch.float32).reshape(slot_count, 1)},
        torch.ones(slot_count, dtype=torch.bool),
        torch.zeros(slot_count, dtype=torch.uint16),
        torch.tensor(episode_index, dtype=torch.uint64),
    )


def _resolved_config(
    run_seed: int,
    *,
    physics_dt: float = 1.0 / 60.0,
    decimation: tuple[int, int] = (1, 1),
) -> ResolvedRunConfig:
    """Build the smallest resolved Config needed to prove seed immutability."""

    return ResolvedRunConfig(
        task_id="test.task",
        task_version="1.0.0",
        session=SessionConfig(mode=LaunchMode.ATTACH),
        worker=WorkerConfig(
            slot_count=2,
            physics_dt=physics_dt,
            decimation=decimation,
            run_seed=run_seed,
        ),
        task=DirectTaskConfig(),
        runner=RslRlRunnerConfig(),
        logging=LoggingConfig(),
        normalized_hash="test-hash",
    )


def _variable_env(
    *,
    run_seed: int,
    decimation: tuple[int, int] = (1, 7),
    physics_dt: float = 0.005,
    max_episode_steps: int = 100_000,
) -> tuple[UERLDirectEnv, _FakeDirectSession, _LifecycleTask]:
    session = _FakeDirectSession(_state([0.2, 0.2]), _post_reset([0.1, 0.1]))
    task = _LifecycleTask(max_episode_steps=max_episode_steps)
    env = UERLDirectEnv(
        session,
        task,
        resolved_config=_resolved_config(
            run_seed,
            physics_dt=physics_dt,
            decimation=decimation,
        ),
    )
    return env, session, task


def test_ac_py_unit_dt_001_002_004_variable_decimation_is_seeded_shared_and_restorable() -> None:
    """Variable N uses both endpoints, isolates RNG, and restores its continuation."""

    with torch.random.fork_rng(devices=[]):
        torch.set_rng_state(torch.Generator(device="cpu").manual_seed(123).get_state())
        expected_global = torch.rand(4)
        torch.set_rng_state(torch.Generator(device="cpu").manual_seed(123).get_state())
        env, session, _task = _variable_env(run_seed=7)
        for _ in range(512):
            env.step(torch.zeros(2, 1))
        observed_global = torch.rand(4)
        assert torch.equal(observed_global, expected_global)
    assert min(session.step_decimations) == 1
    assert max(session.step_decimations) == 7

    replay, replay_session, _replay_task = _variable_env(run_seed=7)
    for _ in range(512):
        replay.step(torch.zeros(2, 1))
    assert session.step_decimations == replay_session.step_decimations

    continuation_state = {"state": env.random_state_dict()}
    restored, restored_session, _restored_task = _variable_env(run_seed=7)
    restored.load_random_state_dict(continuation_state["state"])
    for _ in range(32):
        env.step(torch.zeros(2, 1))
        restored.step(torch.zeros(2, 1))
    assert session.step_decimations[-32:] == restored_session.step_decimations

    fixed, fixed_session, _fixed_task = _variable_env(run_seed=7, decimation=(4, 4))
    before = cast(torch.Tensor, fixed.random_state_dict()["step_decimation_generator"]).clone()
    fixed.step(torch.zeros(2, 1))
    after = cast(torch.Tensor, fixed.random_state_dict()["step_decimation_generator"])
    assert torch.equal(before, after)
    assert fixed_session.step_decimations == [4]

    env.close()
    replay.close()
    restored.close()
    fixed.close()


def test_ac_py_unit_dt_003_reset_rows_observe_dt_min_as_the_completed_frame() -> None:
    """A reset Slot observes DtMin while a survivor observes the completed d_k."""

    session = _FakeDirectSession(_state([1.5, 0.2]), _post_reset([0.1, 0.3]))
    task = _LifecycleTask(max_episode_steps=100)
    env = UERLDirectEnv(
        session,
        task,
        resolved_config=_resolved_config(1, physics_dt=0.005, decimation=(1, 2)),
    )
    env.step(torch.zeros(2, 1))
    env.step(torch.zeros(2, 1))
    assert task.context_transition_dts == [0.01, 0.005]
    assert len(task.observation_control_frame_dts) == 5
    initial_dt = task.observation_control_frame_dts[0]
    terminal_dt = task.observation_control_frame_dts[1]
    after_first_dt = task.observation_control_frame_dts[2]
    assert initial_dt is not None and terminal_dt is not None and after_first_dt is not None
    assert torch.equal(initial_dt, torch.tensor([0.005, 0.005]))
    assert torch.equal(terminal_dt, torch.tensor([0.01, 0.01]))
    assert torch.equal(after_first_dt, torch.tensor([0.005, 0.01]))
    env.close()


def _terrain_resolved_config(run_seed: int, *, num_levels: int) -> ResolvedRunConfig:
    config = _resolved_config(run_seed)
    return ResolvedRunConfig(
        task_id=config.task_id,
        task_version=config.task_version,
        session=config.session,
        worker=WorkerConfig(
            slot_count=2,
            run_seed=run_seed,
            terrain_config={"num_levels": num_levels},
        ),
        task=config.task,
        runner=config.runner,
        logging=config.logging,
        normalized_hash=config.normalized_hash,
    )


def test_initial_policy_state_comes_from_the_first_full_task_reset() -> None:
    """Do not expose the Robot asset's canonical pose as the first episode state."""

    session = _FakeDirectSession(
        _state([0.18, 0.18]),
        _post_reset([0.18, 0.18]),
        initial_post_reset=_post_reset([0.18, 0.18]),
    )
    session.initial = InitialState(
        {"state.value": torch.tensor([[0.049], [0.049]])},
        torch.ones(2, dtype=torch.bool),
        torch.zeros(2, dtype=torch.uint16),
        torch.zeros(2, dtype=torch.uint64),
    )

    env = UERLDirectEnv(session, _LifecycleTask())

    assert session.events == ["initialize", "ready", "reset"]
    assert torch.equal(session.reset_masks[0], torch.ones(2, dtype=torch.bool))
    assert torch.equal(env.get_observations()["policy"], torch.full((2, 1), 0.18))


def _terrain_curriculum_manager(
    *,
    num_levels: int,
    num_envs: int,
    terrain_size_x: float = 10.0,
) -> CurriculumManager:
    """Build a TerrainLevelTerm manager — the only env terrain curriculum path."""

    return CurriculumManager(
        {
            "terrain": TerrainLevelTerm(
                num_levels=num_levels,
                num_envs=num_envs,
                terrain_size_x=terrain_size_x,
            )
        }
    )


def test_terrain_curriculum_does_not_promote_timeout_without_displacement() -> None:
    """A timeout alone is not an Isaac Lab terrain promotion signal."""

    session = _FakeDirectSession(
        _state([0.2, 0.2]),
        _post_reset([0.1, 0.1]),
    )
    env = UERLDirectEnv(
        session,
        _LifecycleTask(max_episode_steps=1),
        resolved_config=_terrain_resolved_config(0, num_levels=3),
        curriculum_manager=_terrain_curriculum_manager(
            num_levels=3, num_envs=2, terrain_size_x=10.0
        ),
    )

    _observations, _rewards, _terminated, truncated, info = env.step(torch.zeros(2, 1))

    assert bool(truncated.all())
    assert torch.equal(info["terrain_level"], torch.zeros(2, dtype=torch.uint16))
    assert info["episode_metrics"]["Curriculum/terrain/next_level"] == 0.0
    terrain_levels = session.reset_terrain_levels[1]
    assert terrain_levels is not None
    assert torch.equal(terrain_levels, torch.zeros(2, dtype=torch.uint16))


def test_ac_py_unit_env_003_resampled_command_scores_only_the_next_action() -> None:
    """Reward reads the command seen by the actor, then the next observation changes."""

    class ChangingCommand:
        def __init__(self) -> None:
            self.velocity = torch.tensor([[0.25, 0.0, 0.0], [0.25, 0.0, 0.0]])

        def channels(self) -> Mapping[str, int]:
            return {"velocity": 3}

        def current(self) -> Mapping[str, torch.Tensor]:
            return {"velocity": self.velocity}

        def reset(self, mask: torch.Tensor, _state: Mapping[str, torch.Tensor]) -> None:
            self.velocity[mask] = torch.tensor([0.25, 0.0, 0.0])

        def note_control_dt(self, _dt: float) -> None:
            self.velocity[:, 0] = 1.0

        def update(self, _state: Mapping[str, torch.Tensor]) -> None:
            pass

    class CommandTask(_LifecycleTask):
        def __init__(self) -> None:
            super().__init__(max_episode_steps=100)
            self.use_command_source(ChangingCommand())

        def compute_rewards(self, context: StepContext, _terminations: TerminationResult) -> torch.Tensor:
            return context.transition_state["velocity"][:, 0].clone()

        def build_observations(
            self, raw_state: Mapping[str, torch.Tensor], state_valid: torch.Tensor,
            previous_policy_actions: torch.Tensor, control_frame_dt: torch.Tensor | None = None,
        ) -> Mapping[str, torch.Tensor]:
            del raw_state, state_valid, previous_policy_actions, control_frame_dt
            velocity = self.published_velocity()
            assert velocity is not None
            return {"policy": velocity[:, :1].clone()}

    curriculum = _OffsetCurriculum()
    env = UERLDirectEnv(
        _FakeDirectSession(_state([0.2, 0.2]), _post_reset([0.1, 0.1])),
        CommandTask(),
        curriculum_manager=CurriculumManager({"probe": curriculum}),
    )
    before = env.get_observations()["policy"]
    after, rewards, _terminated, _truncated, _info = env.step(torch.zeros(2, 1))
    assert torch.equal(before, torch.full((2, 1), 0.25))
    assert torch.equal(rewards, torch.full((2,), 0.25))
    assert torch.equal(after["policy"], torch.full((2, 1), 1.0))
    assert torch.equal(curriculum.steps[0].command_velocity, torch.tensor([[0.25, 0.0], [0.25, 0.0]]))


def test_ac_py_unit_env_004_restoring_terrain_curriculum_starts_on_saved_levels() -> None:
    session = _FakeDirectSession(_state([0.2, 0.2]), _post_reset([0.1, 0.2]))
    env = UERLDirectEnv(
        session,
        _LifecycleTask(),
        resolved_config=_terrain_resolved_config(0, num_levels=3),
        curriculum_manager=_terrain_curriculum_manager(num_levels=3, num_envs=2),
    )
    saved = env.curriculum_state_dict()
    saved["terrain"]["levels"] = [1, 2]

    env.load_curriculum_state_dict(saved)

    assert len(session.reset_terrain_levels) == 2
    restored_levels = session.reset_terrain_levels[-1]
    assert restored_levels is not None
    assert torch.equal(restored_levels, torch.tensor([1, 2], dtype=torch.uint16))
    assert torch.equal(env.get_observations()["policy"], torch.tensor([[0.1], [0.2]]))
    env.close()


def test_ac_py_unit_env_005_terrain_curriculum_does_not_promote_a_fall_without_displacement() -> None:
    session = _FakeDirectSession(
        _state([1.5, 1.5]),
        _post_reset([0.1, 0.1]),
    )
    env = UERLDirectEnv(
        session,
        _OverlappingTerminationTask(max_episode_steps=1),
        resolved_config=_terrain_resolved_config(0, num_levels=2),
        curriculum_manager=_terrain_curriculum_manager(num_levels=2, num_envs=2),
    )

    _observations, _rewards, terminated, truncated, info = env.step(torch.zeros(2, 1))

    assert bool(terminated.all())
    assert bool(truncated.all())
    assert torch.equal(info["terrain_level"], torch.zeros(2, dtype=torch.uint16))
    assert info["episode_metrics"]["Curriculum/terrain/completed_episode_level"] == 0.0
    assert info["episode_metrics"]["Curriculum/terrain/next_level"] == 0.0


def test_terrain_curriculum_does_not_promote_external_truncation_without_a_horizon() -> None:
    session = _FakeDirectSession(
        _state([0.2, 0.2]),
        _post_reset([0.1, 0.1]),
    )
    env = UERLDirectEnv(
        session,
        _OverlappingTerminationTask(max_episode_steps=0),
        resolved_config=_terrain_resolved_config(0, num_levels=2),
        curriculum_manager=_terrain_curriculum_manager(num_levels=2, num_envs=2),
    )

    _observations, _rewards, terminated, truncated, info = env.step(torch.zeros(2, 1))

    assert not bool(terminated.any())
    assert bool(truncated.all())
    assert torch.equal(info["terrain_level"], torch.zeros(2, dtype=torch.uint16))


def test_vc003_step_keeps_terminal_transition_and_sparse_reset() -> None:
    """Verify VC-003 / VC-005: math sees Transition and observations see Post-Reset."""

    session = _FakeDirectSession(_state([1.5, 0.2]), _post_reset([0.1, 0.3]))
    task = _LifecycleTask()
    env = UERLDirectEnv(session, task)

    actions = torch.tensor([[0.7], [0.8]])
    observations, rewards, terminated, truncated, info = env.step(actions)

    assert session.events == ["initialize", "ready", "reset", "step", "reset"]
    assert torch.equal(terminated, torch.tensor([True, False]))
    assert torch.equal(truncated, torch.tensor([False, False]))
    assert torch.equal(rewards, torch.tensor([-2.0, 1.0]))
    assert torch.equal(env.episode_length_buf, torch.tensor([0, 1]))
    assert torch.equal(info["terminal_episode_length"], torch.tensor([1, 1]))
    assert torch.equal(session.reset_masks[0], torch.ones(2, dtype=torch.bool))
    assert torch.equal(session.reset_masks[1], torch.tensor([True, False]))
    assert torch.equal(info["terminal_observation"]["policy"], torch.tensor([[1.5], [0.2]]))
    assert torch.equal(info["terminal_raw_state"]["state.value"], torch.tensor([[1.5], [0.2]]))
    assert torch.equal(observations["policy"], torch.tensor([[0.1], [0.2]]))
    assert torch.equal(task.reward_previous_actions[0], torch.zeros(2, 1))
    assert torch.equal(task.observed_previous_actions[0], torch.zeros(2, 1))
    assert torch.equal(task.observed_previous_actions[1], actions)
    assert torch.equal(task.observed_previous_actions[2], torch.tensor([[0.0], [0.8]]))
    assert task.valid_context_rows == [2]
    print("[VERIFY] VC-003: transition_and_post_reset_sources=PASS sparse_reset=PASS")
    print("[VERIFY] VC-005: terminal_source=transition next_obs_source=post_reset")


def test_policy_trace_distinguishes_input_transition_and_post_reset_state() -> None:
    """A trace row keeps the action input, pre-reset result, and reset state distinct."""

    session = _FakeDirectSession(
        _state([1.5]),
        _post_reset([0.1], episode_index=[1]),
        initial_post_reset=_post_reset([0.4], episode_index=[0]),
    )
    config = _resolved_config(7, physics_dt=0.005, decimation=(2, 2))
    config = replace(config, worker=replace(config.worker, slot_count=1))
    captured: list[Mapping[str, object]] = []
    env = UERLDirectEnv(
        session,
        _LifecycleTask(),
        resolved_config=config,
        step_trace_callback=captured.append,
    )

    observations, rewards, terminated, truncated, _info = env.step(torch.tensor([[0.7]]))

    assert rewards.tolist() == [-2.0]
    assert terminated.tolist() == [True]
    assert truncated.tolist() == [False]
    assert observations["policy"][0, 0].item() == pytest.approx(0.1)
    assert len(captured) == 1
    row = captured[0]
    assert row["phase"] == "post_reset_input"
    assert row["episode_index"] == 0
    assert row["episode_step"] == 0
    assert row["clocks"] == {
        "physics_dt_s": 0.005,
        "input_observation_dt_s": pytest.approx(0.01),
        "step_decimation": 2,
        "transition_dt_s": pytest.approx(0.01),
        "input_episode_elapsed_s": 0.0,
        "transition_episode_elapsed_s": pytest.approx(0.01),
    }
    input_row = row["input"]
    assert isinstance(input_row, Mapping)
    assert input_row["raw_state"]["state.value"] == pytest.approx([0.4])
    assert input_row["state_valid"] is True
    assert input_row["fault_code"] == 0
    assert input_row["observation_groups"]["policy"] == pytest.approx([0.4])
    assert input_row["previous_action"] == [0.0]
    assert input_row["commands"] == {}
    action_row = row["action"]
    assert isinstance(action_row, Mapping)
    assert action_row["policy_action"] == pytest.approx([0.7])
    assert action_row["physical_commands"]["cmd.force"] == pytest.approx([0.7])
    transition_row = row["transition"]
    assert isinstance(transition_row, Mapping)
    transition_state = transition_row["raw_state"]
    assert isinstance(transition_state, Mapping)
    transition_observations = transition_row["observation_groups"]
    assert isinstance(transition_observations, Mapping)
    assert transition_state["state.value"] == pytest.approx([1.5])
    assert transition_observations["policy"] == pytest.approx([1.5])
    assert transition_row["terminated"] is True
    reset_row = row["reset"]
    assert isinstance(reset_row, Mapping)
    assert reset_row["raw_state"]["state.value"] == pytest.approx([0.1])
    assert reset_row["observation_groups"]["policy"] == pytest.approx([0.1])
    assert reset_row["episode_index"] == 1
    env.close()


def test_curriculum_manager_transforms_task_state_and_updates_before_sparse_reset() -> None:
    session = _FakeDirectSession(_state([0.6, 0.2]), _post_reset([0.1, 0.3]))
    term = _OffsetCurriculum()
    env = UERLDirectEnv(
        session,
        _LifecycleTask(),
        curriculum_manager=CurriculumManager({"offset": term}),
    )

    initial = env.get_observations()
    observations, _rewards, terminated, _truncated, info = env.step(torch.zeros(2, 1))

    assert torch.equal(initial["policy"], torch.full((2, 1), 0.5))
    assert torch.equal(terminated, torch.tensor([True, False]))
    assert torch.equal(term.steps[0].transition_state["state.value"], torch.tensor([[0.6], [0.2]]))
    assert torch.equal(term.resets[0], torch.tensor([True, True]))
    assert torch.equal(term.resets[1], torch.tensor([True, False]))
    assert torch.equal(observations["policy"], torch.tensor([[0.6], [0.7]]))
    assert info["episode_metrics"]["Curriculum/offset/completed"].item() == 1.0


def test_vc004_fault_rows_are_not_sent_to_task_math() -> None:
    """Verify VC-004: invalid and fault rows receive fixed handling and still reset."""

    session = _FakeDirectSession(
        _state([99.0, 0.2], valid=[False, True], faults=[9, 0]),
        _post_reset([0.0, 0.2]),
    )
    task = _LifecycleTask()
    env = UERLDirectEnv(session, task)

    _observations, rewards, terminated, truncated, info = env.step(torch.zeros(2, 1))

    assert torch.equal(rewards, torch.tensor([-7.0, 1.0]))
    assert torch.equal(terminated, torch.tensor([True, False]))
    assert not bool(truncated.any())
    # The fault row keeps its fixed reward and terminal snapshot, but only the
    # valid row reaches Task termination/reward mathematics.
    assert task.valid_context_rows == [1]
    assert torch.equal(info["terminal_observation_valid"], torch.tensor([False, True]))
    assert torch.equal(info["terminal_raw_state"]["state.value"], torch.tensor([[99.0], [0.2]]))
    assert torch.equal(info["slot_fault_code"], torch.tensor([9, 0], dtype=torch.uint16))
    print("[VERIFY] VC-004: fault_rows=isolated reward=fault terminated=true")


def test_ac_py_unit_env_001_rewards_use_pre_reset_transition_values() -> None:
    """AC_PY_UNIT_ENV_001: reward/termination see Worker transition, not post-reset."""

    session = _FakeDirectSession(_state([1.5, 0.2]), _post_reset([0.1, 0.3]))
    task = _LifecycleTask()
    env = UERLDirectEnv(session, task)

    _obs, rewards, terminated, _truncated, _info = env.step(torch.zeros(2, 1))

    assert torch.equal(task.reward_transition_values[0], torch.tensor([1.5, 0.2]))
    assert torch.equal(rewards, torch.tensor([-2.0, 1.0]))
    assert torch.equal(terminated, torch.tensor([True, False]))
    # Post-reset values must not have been visible to reward math.
    assert not torch.equal(task.reward_transition_values[0], torch.tensor([0.1, 0.3]))


def test_ac_py_unit_env_006_episode_reward_metrics_are_collected_before_sparse_reset() -> None:
    class LoggedTask(_LifecycleTask):
        def __init__(self) -> None:
            super().__init__()
            self.episode_total = torch.zeros(2)

        def compute_rewards(self, context: StepContext, terminations: TerminationResult) -> torch.Tensor:
            reward = super().compute_rewards(context, terminations)
            assert context.slot_ids is not None
            self.episode_total[context.slot_ids] += reward
            return reward

        def episode_reward_log(self, reset_mask: torch.Tensor) -> Mapping[str, float]:
            assert torch.equal(reset_mask, torch.tensor([True, False]))
            return {"Episode_Reward/track": float(self.episode_total[reset_mask].sum())}

        def on_reset(self, reset_mask: torch.Tensor, post_reset_values: Mapping[str, torch.Tensor]) -> None:
            self.episode_total[reset_mask] = 0.0
            super().on_reset(reset_mask, post_reset_values)

    env = UERLDirectEnv(
        _FakeDirectSession(_state([1.5, 0.2]), _post_reset([0.1, 0.2])),
        LoggedTask(),
    )
    _obs, _rewards, _terminated, _truncated, info = env.step(torch.zeros(2, 1))
    assert info["episode_metrics"]["Episode_Reward/track"] == -2.0


def test_ac_py_unit_env_008_duration_timeout_preserves_surviving_slot_time() -> None:
    """Sparse reset restarts only one Slot's simulated-time episode clock."""

    class TimeTask(_LifecycleTask):
        def compute_terminations(self, context: StepContext) -> TerminationResult:
            value = context.transition_state["state.value"].flatten()
            return TerminationResult(value >= 1.0, time_out_seconds(context, max_seconds=0.03))

    session = _FakeDirectSession(_state([1.5, 0.2]), _post_reset([0.1, 0.2]))
    env = UERLDirectEnv(
        session,
        TimeTask(max_episode_steps=0),
        resolved_config=_resolved_config(0, physics_dt=0.01),
    )
    _, _, terminated, truncated, _ = env.step(torch.zeros(2, 1))
    assert terminated.tolist() == [True, False]
    assert truncated.tolist() == [False, False]

    session.transition = _state([0.2, 0.2])
    _, _, _, truncated, _ = env.step(torch.zeros(2, 1))
    assert truncated.tolist() == [False, False]
    _, _, _, truncated, _ = env.step(torch.zeros(2, 1))
    assert truncated.tolist() == [False, True]


def test_ac_py_unit_env_009_twenty_second_timeout_hits_exact_solver_boundary() -> None:
    """A thousand 20 ms steps must time out at 20 s, not one frame late."""

    class TimeTask(_LifecycleTask):
        def compute_terminations(self, context: StepContext) -> TerminationResult:
            return TerminationResult(
                torch.zeros_like(context.episode_steps, dtype=torch.bool),
                time_out_seconds(context, max_seconds=20.0),
            )

    session = _FakeDirectSession(_state([0.2, 0.2]), _post_reset([0.1, 0.1]))
    env = UERLDirectEnv(
        session,
        TimeTask(max_episode_steps=0),
        resolved_config=_resolved_config(0, physics_dt=0.005, decimation=(4, 4)),
    )
    for _ in range(999):
        _, _, _, truncated, _ = env.step(torch.zeros(2, 1))
        assert not bool(truncated.any())
    _, _, _, truncated, _ = env.step(torch.zeros(2, 1))
    assert truncated.tolist() == [True, True]


def test_ac_py_unit_env_010_random_initial_length_staggers_simulated_time_timeouts() -> None:
    """RSL-RL's random initial episode length offsets the 20 s clock, not step counts."""

    class TimeTask(_LifecycleTask):
        def compute_terminations(self, context: StepContext) -> TerminationResult:
            return TerminationResult(
                torch.zeros_like(context.episode_steps, dtype=torch.bool),
                time_out_seconds(context, max_seconds=20.0),
            )

    slots = 64
    task = TimeTask(max_episode_steps=0)
    task.config = replace(task.config, max_episode_duration_s=20.0)
    env = UERLDirectEnv(
        _FakeDirectSession(_state([0.2] * slots), _post_reset([0.1] * slots)),
        task,
        resolved_config=_resolved_config(0, physics_dt=0.005, decimation=(1, 7)),
    )
    wrapper = UERLVecEnvWrapper(env)
    with torch.random.fork_rng(devices=[]):
        torch.set_rng_state(torch.Generator(device="cpu").manual_seed(0).get_state())
        # The assignment made by rsl_rl OnPolicyRunner.learn(init_at_random_ep_len=True).
        wrapper.episode_length_buf = torch.randint_like(
            wrapper.episode_length_buf, high=int(wrapper.max_episode_length)
        )

    first_timeout_step = torch.full((slots,), -1)
    first_length = torch.full((slots,), -1)
    step = 0
    while bool((first_timeout_step < 0).any()):
        step += 1
        assert step <= 4000
        _, _, _, truncated, info = env.step(torch.zeros(slots, 1))
        first = truncated & (first_timeout_step < 0)
        first_timeout_step[first] = step
        first_length[first] = info["terminal_episode_length"][first]

    assert wrapper.max_episode_length == 4000
    assert torch.equal(first_length, first_timeout_step)
    assert len(set(first_timeout_step.tolist())) > slots // 2
    assert int(first_timeout_step.max() - first_timeout_step.min()) > 500
    env.close()


def test_ac_py_unit_env_002_fault_rows_are_compacted_out_of_task_math() -> None:
    """AC_PY_UNIT_ENV_002: fault/invalid rows never enter compute_rewards context."""

    session = _FakeDirectSession(
        _state([99.0, 0.2], valid=[False, True], faults=[9, 0]),
        _post_reset([0.0, 0.2]),
    )
    task = _LifecycleTask(slot_fault_reward=-7.0)
    env = UERLDirectEnv(session, task)

    _obs, rewards, terminated, _truncated, info = env.step(torch.zeros(2, 1))

    assert task.valid_context_rows == [1]
    assert len(task.reward_transition_values) == 1
    assert task.reward_transition_values[0].numel() == 1
    assert torch.equal(task.reward_transition_values[0], torch.tensor([0.2]))
    assert float(rewards[0].item()) == pytest.approx(-7.0)
    assert torch.equal(terminated, torch.tensor([True, False]))


def test_terminal_observation_backfill_uses_previous_valid_frame() -> None:
    """Ticket 29: invalid-row terminal obs comes from prior valid frame, not fault data."""

    session = _FakeDirectSession(
        _state([99.0, 0.2], valid=[False, True], faults=[9, 0]),
        _post_reset([0.0, 0.2]),
    )
    task = _LifecycleTask()
    env = UERLDirectEnv(session, task)
    initial = env.get_observations()

    _obs, _rewards, _terminated, _truncated, info = env.step(torch.zeros(2, 1))

    # Raw transition still carries the fault payload for diagnostics.
    assert torch.equal(info["terminal_raw_state"]["state.value"], torch.tensor([[99.0], [0.2]]))
    # Terminal observation for the invalid row is the previous valid observation.
    assert torch.equal(
        info["terminal_observation"]["policy"][0],
        initial["policy"][0],
    )
    assert not torch.equal(info["terminal_observation"]["policy"][0], torch.tensor([99.0]))


def test_ac003_mixed_slots_cover_termination_timeout_and_fault() -> None:
    """Verify AC-003: one four-slot run covers every terminal category and sparse masks."""

    session = _FakeDirectSession(
        _state([1.5, 0.2, 0.2, 0.2], valid=[True, True, False, True], faults=[0, 0, 9, 0]),
        _post_reset([0.1, 0.2, 0.3, 0.4], episode_index=[1, 0, 1, 0]),
    )
    task = _LifecycleTask(max_episode_steps=2)
    env = UERLDirectEnv(session, task)

    _first_obs, first_rewards, first_terminated, first_truncated, _first_info = env.step(torch.zeros(4, 1))
    _second_obs, second_rewards, second_terminated, second_truncated, _second_info = env.step(torch.zeros(4, 1))

    assert torch.equal(first_rewards, torch.tensor([-2.0, 1.0, -7.0, 1.0]))
    assert torch.equal(first_terminated, torch.tensor([True, False, True, False]))
    assert not bool(first_truncated.any())
    assert torch.equal(session.reset_masks[0], torch.ones(4, dtype=torch.bool))
    assert torch.equal(session.reset_masks[1], torch.tensor([True, False, True, False]))
    assert torch.equal(second_rewards, torch.tensor([-2.0, 1.0, -7.0, 1.0]))
    assert torch.equal(second_terminated, torch.tensor([True, False, True, False]))
    assert torch.equal(second_truncated, torch.tensor([False, True, False, True]))
    assert torch.equal(session.reset_masks[2], torch.ones(4, dtype=torch.bool))
    assert task.valid_context_rows == [3, 3]
    print("[VERIFY] AC-003: mixed_termination_timeout_fault=sparse_reset=PASS")


def test_vc006_reset_all_slots_zeroes_counter_and_close_is_idempotent() -> None:
    """Verify VC-006: explicit reset uses one full mask and close has one ownership call."""

    session = _FakeDirectSession(_state([0.0, 0.0]), _post_reset([0.1, 0.2]))
    resolved_config = _resolved_config(run_seed=123)
    env = UERLDirectEnv(session, _LifecycleTask(), resolved_config=resolved_config)

    env.episode_length_buf[:] = 4
    run_seed_before = env.resolved_config.worker.run_seed if env.resolved_config is not None else None
    _observations, info = env.reset(seed=987)
    env.close()
    env.close()

    assert len(session.reset_masks) == 2
    assert torch.equal(session.reset_masks[0], torch.tensor([True, True]))
    assert torch.equal(session.reset_masks[1], torch.tensor([True, True]))
    assert torch.equal(env.episode_length_buf, torch.zeros(2, dtype=torch.long))
    assert torch.equal(info["episode_index"], torch.tensor([1, 0], dtype=torch.uint64))
    assert env.resolved_config is not None
    assert env.resolved_config.worker.run_seed == run_seed_before == 123
    assert session.close_calls == 1
    print("[VERIFY] VC-006: reset_calls=1 episode_steps=0 seed_unchanged=true")


def test_vc002_initialization_failure_closes_session() -> None:
    """Verify VC-002: a failed initialize cannot expose a half-ready environment."""

    class FailingSession:
        """Reject Initialize so the environment's close-on-construction path is observable."""

        num_slots = 1
        close_calls = 0
        descriptor: Mapping[str, Any] = {}

        @property
        def last_step_timing(self) -> Mapping[str, float]:
            return {}

        @property
        def last_reset_timing(self) -> Mapping[str, float]:
            return {}

        def initialize_with_robot_spec(
            self,
            schema: SessionSchema,
            bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
        ) -> InitialState:
            """Fail before Ready to model a Worker initialization error."""

            del schema, bind_robot_spec
            raise RuntimeError("initialize failed")

        def acknowledge_ready(self) -> None:
            """Fail the test if Ready is attempted after Initialize failure."""

            raise AssertionError("ready must not run")

        def step(self, command: PhysicalCommandBatch, step_decimation: int | None = None) -> TransitionState:
            """Fail the test if Step is attempted on a half-ready environment."""

            del command, step_decimation
            raise AssertionError("step must not run")

        def reset(
            self,
            reset_mask: torch.Tensor,
            terrain_levels: torch.Tensor | None = None,
            reset_values: torch.Tensor | None = None,
        ) -> PostResetState:
            """Fail the test if Reset is attempted on a half-ready environment."""

            del reset_mask, terrain_levels, reset_values
            raise AssertionError("reset must not run")

        def apply_event(self, effect: EventEffect) -> Mapping[str, Any]:
            """Fail the test if an event is attempted on a half-ready environment."""

            del effect
            raise AssertionError("event must not run")

        def close(self, reason: str = "session_close") -> None:
            """Count cleanup after the failed construction."""

            self.close_calls += 1

    session = FailingSession()
    with pytest.raises(RuntimeError, match="initialize failed"):
        UERLDirectEnv(session, _LifecycleTask())

    assert session.close_calls == 1
    print("[VERIFY] VC-002: ready_before_reset=true init_failure_close=1")


def test_vc002_ready_failure_closes_session() -> None:
    """Verify VC-002: a failed ReadyAck also closes the typed Session."""

    class ReadyFailingSession(_FakeDirectSession):
        """Fail the Ready transition after returning a valid InitialState."""

        def acknowledge_ready(self) -> None:
            """Reject Ready so construction cleanup can be asserted."""

            raise RuntimeError("ready failed")

    session = ReadyFailingSession(_state([0.0, 0.0]), _post_reset([0.0, 0.0]))
    with pytest.raises(RuntimeError, match="ready failed"):
        UERLDirectEnv(session, _LifecycleTask())

    assert session.close_calls == 1
    print("[VERIFY] VC-002: ready_failure_close=1 step_blocked=true")


def test_startup_event_randomizes_terrain_levels_before_first_reset() -> None:
    """Ticket 30: startup events run after Ready and stage terrain for first reset."""

    from uerl.core.config.robot import ConstraintFrame, JointTopology, RobotSpec, RobotTopology
    from uerl.core.mdp.lib.events import randomize_initial_terrain_levels
    from uerl.core.mdp.managers.event import EventManager
    from uerl.core.mdp.terms import EventCfg, EventTermCfg

    frame = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))
    topology = RobotTopology(
        body_names=("base", "cart", "pole"),
        body_motion_types=("kinematic", "simulated", "simulated"),
        root_body_index=0,
        fixed_base=True,
        joints=(
            JointTopology("cart", 0, 1, 1, "linear_x", "prismatic", "m", None, None, frame, frame, 0.0),
            JointTopology("pole", 1, 2, 1, "twist", "revolute", "rad", -3.14, 3.14, frame, frame, 0.0),
        ),
    )
    spec = RobotSpec(actuators=(), observations=(), reset=(), body_names=topology.body_names, topology=topology)
    generator = torch.Generator(device="cpu")
    generator.manual_seed(11)
    events = EventManager(
        EventCfg(
            terms={
                "boot_levels": EventTermCfg(
                    func=randomize_initial_terrain_levels,
                    mode="startup",
                    params={"num_levels": 8},
                )
            }
        ),
        spec,
        batch_size=2,
        device="cpu",
        generator=generator,
    )
    session = _FakeDirectSession(_state([0.2, 0.2]), _post_reset([0.1, 0.1]))
    env = UERLDirectEnv(
        session,
        _LifecycleTask(),
        resolved_config=_terrain_resolved_config(0, num_levels=8),
        event_manager=events,
    )

    assert session.events == ["initialize", "ready", "reset"]
    first_levels = session.reset_terrain_levels[0]
    assert first_levels is not None
    assert first_levels.shape == (2,)
    levels_i = first_levels.to(dtype=torch.int64)
    assert bool((levels_i >= 0).all() and (levels_i < 8).all())
    env.close()


def test_startup_event_effect_is_flushed_before_first_reset() -> None:
    """Ticket35: a scene effect reaches Session before the first reset."""

    from uerl.core.config.robot import RobotSpec, RobotTopology
    from uerl.core.mdp.lib.events import push_root
    from uerl.core.mdp.managers.event import EventManager
    from uerl.core.mdp.terms import EventCfg, EventTermCfg

    spec = RobotSpec(
        actuators=(),
        observations=(),
        reset=(),
        body_names=("root",),
        topology=RobotTopology(("root",), ("simulated",), 0, False, ()),
    )
    generator = torch.Generator(device="cpu")
    generator.manual_seed(13)
    events = EventManager(
        EventCfg(
            terms={
                "push": EventTermCfg(
                    func=push_root,
                    mode="startup",
                    params={"velocity_range_mps": (0.25, 0.25)},
                )
            }
        ),
        spec,
        batch_size=2,
        device="cpu",
        generator=generator,
    )
    session = _FakeDirectSession(_state([0.2, 0.2]), _post_reset([0.1, 0.1]))
    env = UERLDirectEnv(session, _LifecycleTask(), event_manager=events)

    assert session.events == ["initialize", "ready", "event:root_push", "reset"]
    assert [effect.kind for effect in session.applied_events] == ["root_push"]
    assert torch.equal(session.applied_events[0].values, torch.full((2, 3), 0.25))
    env.close()


def test_ac_py_unit_env_007_interval_event_reaches_session_after_sparse_reset() -> None:
    """An interval push belongs to the new episode when a Slot terminates."""

    from uerl.core.config.robot import RobotTopology
    from uerl.core.mdp.lib.events import push_root
    from uerl.core.mdp.managers.event import EventManager
    from uerl.core.mdp.terms import EventCfg, EventTermCfg

    spec = RobotSpec(
        actuators=(), observations=(), reset=(), body_names=("root",),
        topology=RobotTopology(("root",), ("simulated",), 0, False, ()),
    )
    generator = torch.Generator(device="cpu")
    generator.manual_seed(13)
    events = EventManager(
        EventCfg(terms={"push": EventTermCfg(
            func=push_root, mode="interval", interval_range_s=(0.001, 0.001),
            params={"velocity_range_mps": (0.25, 0.25)},
        )}),
        spec, batch_size=2, device="cpu", generator=generator,
    )
    session = _FakeDirectSession(_state([1.5, 0.2]), _post_reset([0.1, 0.2]))
    env = UERLDirectEnv(session, _LifecycleTask(), event_manager=events)
    env.step(torch.zeros(2, 1))

    assert session.events == ["initialize", "ready", "reset", "step", "reset", "event:root_push"]
    assert torch.equal(session.applied_events[0].mask, torch.tensor([True, True]))
    env.close()


def test_event_manager_factory_binds_after_reflection_and_before_startup() -> None:
    """A topology-dependent EventManager is created only after RobotSpec binding."""

    from uerl.core.config.robot import RobotSpec, RobotTopology
    from uerl.core.mdp.lib.events import push_root
    from uerl.core.mdp.managers.event import EventManager
    from uerl.core.mdp.terms import EventCfg, EventTermCfg

    spec = RobotSpec(
        actuators=(),
        observations=(),
        reset=(),
        body_names=("root",),
        topology=RobotTopology(("root",), ("simulated",), 0, False, ()),
    )

    class FactorySession(_FakeDirectSession):
        """Call the DirectEnv binding callback with one reflected RobotSpec."""

        def initialize_with_robot_spec(
            self,
            schema: SessionSchema,
            bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
        ) -> InitialState:
            del schema
            self._record("initialize")
            self.descriptor = {"available_state_schema": []}
            bind_robot_spec(spec, ObservationShapeTable({}))
            return self.initial

    factory_specs: list[RobotSpec] = []

    def build_events(bound_spec: RobotSpec) -> EventManager:
        factory_specs.append(bound_spec)
        generator = torch.Generator(device="cpu")
        generator.manual_seed(13)
        return EventManager(
            EventCfg(
                terms={
                    "push": EventTermCfg(
                        func=push_root,
                        mode="startup",
                        params={"velocity_range_mps": (0.25, 0.25)},
                    )
                }
            ),
            bound_spec,
            batch_size=2,
            device="cpu",
            generator=generator,
        )

    session = FactorySession(_state([0.2, 0.2]), _post_reset([0.1, 0.1]))
    env = UERLDirectEnv(session, _LifecycleTask(), event_manager_factory=build_events)

    assert factory_specs == [spec]
    assert session.events == ["initialize", "ready", "event:root_push", "reset"]
    env.close()

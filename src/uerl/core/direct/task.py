"""Define the framework-independent DirectTask contract."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass, replace
from typing import TYPE_CHECKING, Protocol, cast, runtime_checkable

import torch

from uerl.core.direct.robot_observation import robot_observation_schema
from uerl.core.mdp.executor import PlanInputs
from uerl.core.mdp.managers.action import ActionManager
from uerl.core.mdp.managers.observation import ObservationManager
from uerl.core.mdp.managers.reward import RewardManager
from uerl.core.mdp.managers.termination import TerminationManager
from uerl.core.mdp.plan import ActionPlan, ObservationPlan
from uerl.core.mdp.terms import ActionCfg, CurriculumCfg, ObservationCfg, RewardCfg, TerminationCfg
from uerl.errors import ConfigError

from ..config import DirectTaskConfig
from ..config.robot import RobotSpec
from .capabilities import CapabilityStatus, TaskCapabilities, TaskCapability
from .curriculum import CurriculumManager
from .robot_action import ROBOT_ACTUATOR_TARGET_FIELD, robot_actuator_action_schema
from .robot_observation import ObservationShapeTable
from .termination import TerminationTerm, combine_termination_terms
from .types import PhysicalCommandBatch, SessionSchema, StepContext, TerminationResult

if TYPE_CHECKING:
    from uerl.core.mdp.managers.event import EventManager

    from .curriculum import CurriculumStep


class CommandSource(Protocol):
    """Supplies named command channels for observation ``command`` ops."""

    def current(self) -> Mapping[str, torch.Tensor]:
        """Return the latest command tensors keyed by channel name."""

    def channels(self) -> Mapping[str, int]:
        """Return declared channel name → width (assemble-time contract)."""


@runtime_checkable
class CurriculumBindableCommandSource(Protocol):
    """CommandSource that consumes curriculum latch channels."""

    def bind_curriculum(self, source: CommandSource) -> None:
        """Attach the curriculum manager's latch ``CommandSource``."""


@dataclass(frozen=True, slots=True)
class EmptyCommandSource:
    """CartPole-style empty command source — no channels."""

    def current(self) -> Mapping[str, torch.Tensor]:
        return {}

    def channels(self) -> Mapping[str, int]:
        return {}


@dataclass(frozen=True, slots=True)
class DirectTaskCfg:
    """Declarative managers that assemble one DirectTask implementation."""

    task: DirectTaskConfig
    actions: ActionCfg
    observations: ObservationCfg
    terminations: TerminationCfg
    rewards: RewardCfg
    curriculum: CurriculumCfg | None = None
    terrain_config_path: str | None = None


class DirectTask:
    """Assemble one manager-owned Task without owning a Worker or Bridge connection.

    All task methods use batch-first tensors and stable Slot ordering. The Direct
    environment compacts invalid or faulted rows out of termination, reward, and
    metric mathematics, while observation building receives the full batch plus
    its validity mask. The task must not perform Session, Worker, Bridge, reset,
    or device lifecycle operations.
    """

    def __init__(
        self,
        config: DirectTaskConfig | DirectTaskCfg | None = None,
        robot_spec: RobotSpec | None = None,
        *,
        control_dt: float = 1.0,
        batch_size: int = 1,
        device: str | torch.device = "cpu",
        command_source: CommandSource | None = None,
        cfg_factory: Callable[[RobotSpec], DirectTaskCfg] | None = None,
        task_config: DirectTaskConfig | None = None,
        observation_shapes: ObservationShapeTable | None = None,
    ) -> None:
        """Configure task-owned state and optionally assemble its managers.

        ``config`` is the generic task configuration used by subclasses that
        own their own mathematics. A ``DirectTaskCfg`` or ``cfg_factory`` turns
        on manager assembly; the reflected ``RobotSpec`` is then required before
        schema, action, observation, termination, or reward use.
        """

        if control_dt <= 0.0:
            raise ConfigError(
                f"control_dt must be positive, got {control_dt}",
                code="CONFIG_OUT_OF_RANGE",
                path="control_dt",
            )
        if batch_size <= 0:
            raise ConfigError(
                f"batch_size must be positive, got {batch_size}",
                code="CONFIG_OUT_OF_RANGE",
                path="batch_size",
            )
        if isinstance(config, DirectTaskCfg) and cfg_factory is not None:
            raise ConfigError(
                "DirectTask accepts only one of cfg or cfg_factory",
                code="CONFIG_OUT_OF_RANGE",
                path="cfg",
            )
        manager_cfg = config if isinstance(config, DirectTaskCfg) else None
        initial = (
            manager_cfg.task
            if manager_cfg is not None
            else config
            if isinstance(config, DirectTaskConfig)
            else task_config
            if task_config is not None
            else DirectTaskConfig()
        )
        self.config = initial
        self._curriculum_manager: CurriculumManager | None = None
        self._event_manager: EventManager | None = None
        self._cfg = manager_cfg
        self._cfg_factory = cfg_factory
        self.robot_spec = robot_spec
        self.observation_shapes = observation_shapes
        self.control_dt = float(control_dt)
        self._batch_size = int(batch_size)
        self._device = device
        self._command_source: CommandSource = (
            EmptyCommandSource() if command_source is None else command_source
        )
        self._actions: ActionManager | None = None
        self._observations: ObservationManager | None = None
        self._terminations: TerminationManager | None = None
        self._rewards: RewardManager | None = None
        if manager_cfg is not None or cfg_factory is not None:
            if robot_spec is not None:
                self._assemble(robot_spec, observation_shapes)

    @property
    def state_requirements(self) -> tuple[str, ...]:
        """Return the stable names of raw Worker fields consumed by the Task."""

        return self.config.state_requirements

    @property
    def action_schema(self) -> tuple[str, ...]:
        """Return the stable names of physical command fields produced by the Task."""

        return self.config.action_schema

    @property
    def num_actions(self) -> int:
        """Return the default policy action width for this command schema."""

        if self._actions is not None:
            return self._actions.policy_width
        return len(self.action_schema)

    @property
    def schema(self) -> SessionSchema:
        """Return complete typed State and Action descriptors for the Bridge boundary.

        Returns:
            A frozen schema whose descriptor names, dtypes, shapes, units, frames,
            semantics, and sources are sufficient for one Session initialization.
        """

        if self.robot_spec is None:
            return SessionSchema((), ())
        return SessionSchema(
            robot_observation_schema(self.robot_spec, self._require_observation_shapes()),
            robot_actuator_action_schema(self.robot_spec),
        )

    def bind_robot_spec(
        self,
        robot_spec: RobotSpec,
        *,
        observation_shapes: ObservationShapeTable | None = None,
    ) -> SessionSchema:
        """Bind reflected Robot semantics before committing the Task schema."""

        if self._cfg is None and self._cfg_factory is None:
            self.robot_spec = robot_spec
            self.observation_shapes = observation_shapes
            return self.schema
        self._assemble(robot_spec, observation_shapes)
        return self.schema

    @property
    def observation_groups(self) -> Mapping[str, tuple[str, ...]]:
        """Return the named observation groups exposed to training adapters.

        Returns:
            A mapping from adapter-visible group names to the ordered raw fields
            used to construct each batch-first observation tensor.
        """

        if self.robot_spec is None:
            return {"policy": ()}
        return {
            "policy": tuple(
                cast(str, item["name"])
                for item in robot_observation_schema(self.robot_spec, self._require_observation_shapes())
            )
        }

    @property
    def max_episode_steps(self) -> int:
        """Return the task timeout in Python steps, or zero when not configured.

        Returns:
            A non-negative step count. Zero means the task does not request a
            Python-side timeout.
        """

        return self.config.max_episode_steps

    @property
    def max_episode_duration_s(self) -> float | None:
        """Return the configured simulated-time episode horizon, if any."""

        return self.config.max_episode_duration_s

    @property
    def slot_fault_reward(self) -> float:
        """Return the fixed reward for a Worker-reported Slot fault.

        Returns:
            The finite reward assigned to every invalid or faulted row before
            the environment resets that row.
        """

        return self.config.slot_fault_reward

    def preprocess_actions(
        self,
        policy_actions: torch.Tensor,
        raw_state: Mapping[str, torch.Tensor],
    ) -> PhysicalCommandBatch:
        """Map policy actions to named physical commands without touching UE state.

        Args:
            policy_actions: Batch-first policy output aligned with the full stable
                Slot order. The first dimension must match the Session batch.
            raw_state: Named raw Worker fields from the state immediately before
                the step, using the same Slot order and batch dimension.

        Returns:
            Named physical command tensors with the same Slot alignment and
            shapes declared by ``action_schema``.

        Side effects:
            None on the Session, Worker, or input mappings. Task implementations
            may allocate result tensors but must not reset or mutate environment
            lifecycle state.
        """

        if self._actions is None:
            raise RuntimeError("DirectTask must bind RobotSpec before action preprocessing")
        actions = self._actions.process(policy_actions)
        packed = torch.cat(
            [actions[name] for name in self._actions.plan.command_fields],
            dim=-1,
        )
        if packed.shape[-1] != self._actions.policy_width:
            raise ConfigError(
                "packed actuator command width does not match policy_width",
                code="CONFIG_OUT_OF_RANGE",
                path="preprocess_actions",
            )
        return PhysicalCommandBatch({ROBOT_ACTUATOR_TARGET_FIELD: packed})

    def build_observations(
        self,
        raw_state: Mapping[str, torch.Tensor],
        state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor,
        control_frame_dt: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        """Build named observation groups from state, validity, and previous actions.

        Args:
            raw_state: Named state tensors in stable Slot order. The first
                dimension is the number of rows being observed.
            state_valid: Boolean validity mask aligned with ``raw_state`` rows.
                Invalid rows must not be interpreted as valid task observations.
            previous_policy_actions: Policy actions used for the preceding
                transition, or zeros at the start of an episode.
            control_frame_dt: The just-completed control interval for each row,
                or ``None`` when the observation plan does not consume it.

        Returns:
            A mapping of adapter-visible observation group names to batch-first
            tensors. Group names and feature order must remain stable for a Run.

        Side effects:
            None. The method must not modify the input tensors or perform reset,
            Session, or Worker operations.
        """

        if self._observations is None:
            raise RuntimeError("DirectTask must bind RobotSpec before observation building")
        del state_valid
        self.refresh_command(raw_state)
        return self._observations.compute(
            PlanInputs(
                raw_state=raw_state,
                commands=dict(self._command_source.current()),
                control_frame_dt=(
                    None if control_frame_dt is None else control_frame_dt.reshape(-1, 1)
                ),
                previous_action=previous_policy_actions,
            )
        )

    @property
    def termination_terms(self) -> tuple[TerminationTerm, ...]:
        """Return the named termination terms owned by this Task."""

        return ()

    def compute_terminations(self, context: StepContext) -> TerminationResult:
        """Evaluate this Task's named terms with separate timeout semantics."""

        if self._terminations is not None:
            return self._terminations.compute(context)
        return combine_termination_terms(self.termination_terms, context)

    def compute_rewards(
        self,
        context: StepContext,
        terminations: TerminationResult,
    ) -> torch.Tensor:
        """Compute rewards from the transition state before any reset is applied.

        Args:
            context: The same valid-row Transition context passed to termination
                mathematics. Its values are pre-reset state values.
            terminations: The task-owned masks for those same rows and in the same
                order.

        Returns:
            One finite reward per context row. The result is scattered back to
            stable Slot order by the Direct environment.

        Side effects:
            None. The method must not reset rows or replace transition data.
        """

        if self._rewards is None:
            raise NotImplementedError("DirectTask subclasses must implement compute_rewards")
        return self._rewards.compute(self.context_with_command(context), terminations)

    def collect_metrics(
        self,
        context: StepContext,
        terminations: TerminationResult,
    ) -> Mapping[str, torch.Tensor | float]:
        """Return optional metrics without changing task control flow.

        Args:
            context: The valid-row transition context used by the task.
            terminations: The corresponding termination and timeout masks.

        Returns:
            Scalar metrics or batch metrics keyed by stable metric names. An
            empty mapping means that the task exposes no optional metrics.
            Default naming publishes per-term ``Episode_Termination/<name>``
            masks so the environment does not invent metric namespaces.

        Side effects:
            None. Metrics must not alter rewards, masks, reset decisions, or
            Session state.
        """

        del context
        return {
            f"Episode_Termination/{name}": value.to(dtype=torch.float32)
            for name, value in terminations.reason.items()
            if isinstance(value, torch.Tensor) and value.shape == terminations.terminated.shape
        }

    def bind_curriculum_manager(self, manager: CurriculumManager) -> None:
        """Attach the optional curriculum manager for ``on_step`` / ``on_reset``."""

        self._curriculum_manager = manager
        bind = getattr(self._command_source, "bind_curriculum_manager", None)
        if callable(bind):
            bind(manager)

    def bind_curriculum_commands(self, source: CommandSource) -> None:
        """Wire caller-supplied latch tensors into a command item that accepts them."""

        bind = getattr(self._command_source, "bind_curriculum", None)
        if callable(bind):
            bind(source)

    def use_command_source(self, source: CommandSource) -> None:
        """Replace the command publisher before the observation plan is assembled."""

        if self._observations is not None:
            raise RuntimeError("command source is fixed once the task is assembled")
        self._command_source = source

    def align_batch(self, batch_size: int) -> None:
        """Match task-owned batch buffers to the Session Slot count before assembly."""

        if batch_size <= 0:
            raise ConfigError(
                f"batch_size must be positive, got {batch_size}",
                code="CONFIG_OUT_OF_RANGE",
                path="batch_size",
            )
        self._batch_size = int(batch_size)

    def refresh_command(
        self,
        raw_state: Mapping[str, torch.Tensor],
        *,
        dt: float | None = None,
    ) -> None:
        """Refresh the published command. ``dt`` advances a source that resamples."""

        if dt is not None:
            note_dt = getattr(self._command_source, "note_control_dt", None)
            if callable(note_dt):
                note_dt(dt)
        update = getattr(self._command_source, "update", None)
        if callable(update):
            update(raw_state)

    def published_velocity(self) -> torch.Tensor | None:
        """Return the ``velocity`` channel when this task publishes one."""

        velocity = self._command_source.current().get("velocity")
        if not isinstance(velocity, torch.Tensor):
            return None
        return velocity

    @property
    def command_source(self) -> CommandSource:
        """Return the source currently bound to the Task's command channels."""

        return self._command_source

    def command_channels(self) -> Mapping[str, int]:
        """Return the assemble-time channel-width contract for the source."""

        return dict(self._command_source.channels())

    def command_planar_speed(self) -> torch.Tensor | None:
        """Planar speed of the command that scored the latest refresh."""

        velocity = self.published_velocity()
        if velocity is None:
            return None
        return cast(torch.Tensor, torch.linalg.vector_norm(velocity[:, :2], dim=1))

    @property
    def capabilities(self) -> TaskCapabilities:
        """Report known train/evaluate support and plan-backed export support.

        Training and evaluation use the same DirectEnv contract. A Manager
        configuration remains unknown until it is assembled against the
        Worker-provided RobotSpec. Python-only Tasks can be trainable and
        evaluable, but they have no export representation unless they expose
        Manager-generated plans. A custom execution override beside Manager
        plans is reported as unknown because this check does not prove that its
        mathematics matches the serialized plan.
        """

        manager_requested = self._cfg is not None or self._cfg_factory is not None
        manager_ready = (
            self._actions is not None
            and self._observations is not None
            and self._terminations is not None
            and self._rewards is not None
        )
        custom_action = type(self).preprocess_actions is not DirectTask.preprocess_actions
        custom_observation = type(self).build_observations is not DirectTask.build_observations
        custom_reward = type(self).compute_rewards is not DirectTask.compute_rewards
        custom_termination = (
            type(self).compute_terminations is not DirectTask.compute_terminations
            or type(self).termination_terms is not DirectTask.termination_terms
        )

        if manager_requested and not manager_ready:
            runtime_capability = TaskCapability(
                CapabilityStatus.UNKNOWN,
                "Manager declarations require binding to the Worker RobotSpec before they can be checked",
            )
        else:
            missing = []
            if self._actions is None and not custom_action:
                missing.append("preprocess_actions")
            if self._observations is None and not custom_observation:
                missing.append("build_observations")
            if self._rewards is None and not custom_reward:
                missing.append("compute_rewards")
            if self._terminations is None and not custom_termination:
                missing.append("compute_terminations or termination_terms")
            if missing:
                runtime_capability = TaskCapability(
                    CapabilityStatus.UNSUPPORTED,
                    "Task must provide " + ", ".join(missing) + " through DirectTask methods or Managers",
                )
            else:
                runtime_capability = TaskCapability(CapabilityStatus.SUPPORTED)

        custom_execution = tuple(
            name
            for name, overridden in (
                ("preprocess_actions", custom_action),
                ("build_observations", custom_observation),
            )
            if overridden
        )
        custom_plan = (
            type(self).observation_plan is not DirectTask.observation_plan
            or type(self).action_plan is not DirectTask.action_plan
        )
        if custom_execution and manager_ready:
            plan_capability = TaskCapability(
                CapabilityStatus.UNKNOWN,
                "Python execution override(s) "
                + ", ".join(custom_execution)
                + " may differ from Manager plans; plan equivalence is not established",
            )
        elif manager_requested and not manager_ready:
            plan_capability = TaskCapability(
                CapabilityStatus.UNKNOWN,
                "observation/action plans are checked after binding the Worker RobotSpec",
            )
        elif manager_ready and not custom_plan:
            plan_capability = TaskCapability(CapabilityStatus.SUPPORTED)
        elif custom_plan:
            plan_capability = TaskCapability(
                CapabilityStatus.UNKNOWN,
                "custom plan properties are present, but their execution equivalence is not established",
            )
        else:
            plan_capability = TaskCapability(
                CapabilityStatus.UNSUPPORTED,
                "Python Task action/observation methods have no Manager-generated "
                "observation_plan/action_plan; "
                "use Manager action and observation declarations for export",
            )

        return TaskCapabilities(
            train=runtime_capability,
            evaluate=runtime_capability,
            export=plan_capability,
        )

    def context_with_command(self, context: StepContext) -> StepContext:
        """Attach the published velocity so reward terms read one channel."""

        if "velocity" in context.transition_state:
            return context
        self.refresh_command(context.transition_state)
        velocity = self.published_velocity()
        if velocity is None:
            return context
        return replace(
            context,
            transition_state={**dict(context.transition_state), "velocity": velocity},
        )

    def bind_event_manager(self, manager: EventManager) -> None:
        """Attach the optional event manager (env drives apply timing)."""

        self._event_manager = manager

    def on_step(self, step: CurriculumStep) -> Mapping[str, torch.Tensor | float]:
        """Advance task-owned managers after one transition (curriculum, etc.)."""

        manager = self._curriculum_manager
        if manager is None:
            return {}
        return manager.update(step)

    def episode_reward_log(self, reset_mask: torch.Tensor) -> Mapping[str, float]:
        """Read completed episode reward totals before selected Slots reset."""

        if self._rewards is None:
            return {}
        return self._rewards.episode_log(reset_mask)

    def on_reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_values: Mapping[str, torch.Tensor],
    ) -> None:
        """Notify task-owned managers after selected Slots enter post-reset state."""

        manager = self._curriculum_manager
        if manager is not None:
            manager.reset(reset_mask, post_reset_values)
        if self._rewards is not None:
            self._rewards.reset(reset_mask)
        reset_command = getattr(self._command_source, "reset", None)
        if callable(reset_command):
            reset_command(reset_mask, post_reset_values)


    def _assemble(
        self,
        robot_spec: RobotSpec,
        observation_shapes: ObservationShapeTable | None,
    ) -> None:
        """Build managers in fixed order: action → observation → termination → reward."""

        if observation_shapes is None:
            raise ConfigError(
                "DirectTask requires the UE observation shape table before binding RobotSpec",
                code="ROBOT_OBSERVATION_SHAPE_MISSING",
                path="robot.observation_shapes",
            )
        if self._cfg is not None:
            cfg = self._cfg
        elif self._cfg_factory is not None:
            cfg = self._cfg_factory(robot_spec)
        else:
            raise ConfigError(
                "DirectTask requires cfg or cfg_factory",
                code="CONFIG_MISSING_FIELD",
                path="cfg",
            )
        self._cfg = cfg
        self.robot_spec = robot_spec
        self.observation_shapes = observation_shapes
        self._actions = ActionManager(cfg.actions, robot_spec)
        # Assemble-time contract: CommandSource.channels() is authoritative (AC_002).
        declared = self._command_source.channels()
        command_channels = {name: int(width) for name, width in declared.items()}
        # Empty source: omit channel map so plans without ``command`` still compile;
        # a ``command`` op fails at PlanExecutor construction when the channel is
        # absent (plan-time, not a silent zero vector).
        self._observations = ObservationManager(
            cfg.observations,
            robot_spec,
            policy_width=self._actions.policy_width,
            observation_shapes=observation_shapes,
            command_channels=command_channels or None,
        )
        self._terminations = TerminationManager(cfg.terminations, robot_spec)
        self._rewards = RewardManager(
            cfg.rewards,
            robot_spec,
            batch_size=self._batch_size,
            device=self._device,
        )
        self._refresh_schema_config()

    def _refresh_schema_config(self) -> None:
        if self.robot_spec is None:
            return
        state_names = tuple(
            cast(str, item["name"])
            for item in robot_observation_schema(self.robot_spec, self._require_observation_shapes())
        )
        action_names = tuple(
            cast(str, item["name"]) for item in robot_actuator_action_schema(self.robot_spec)
        )
        self.config = replace(
            self.config,
            state_requirements=state_names,
            action_schema=action_names,
        )

    def _require_observation_shapes(self) -> ObservationShapeTable:
        if self.observation_shapes is None:
            raise RuntimeError("DirectTask requires UE observation shapes before schema use")
        return self.observation_shapes

    def _require_actions(self) -> ActionManager:
        if self._actions is None:
            raise RuntimeError("DirectTask must bind RobotSpec before action use")
        return self._actions

    def _require_observations(self) -> ObservationManager:
        if self._observations is None:
            raise RuntimeError("DirectTask must bind RobotSpec before observation use")
        return self._observations

    def _require_terminations(self) -> TerminationManager:
        if self._terminations is None:
            raise RuntimeError("DirectTask must bind RobotSpec before termination use")
        return self._terminations

    def _require_rewards(self) -> RewardManager:
        if self._rewards is None:
            raise RuntimeError("DirectTask must bind RobotSpec before reward use")
        return self._rewards

    @property
    def observation_plan(self) -> ObservationPlan:
        """Return the assembled policy observation plan."""

        return self._require_observations().plan

    @property
    def action_plan(self) -> ActionPlan:
        """Return the assembled physical action plan."""

        return self._require_actions().plan


__all__ = [
    "CommandSource",
    "CurriculumBindableCommandSource",
    "DirectTask",
    "DirectTaskCfg",
    "EmptyCommandSource",
]

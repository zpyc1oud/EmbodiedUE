"""Implement explicit, stable Task registration and lookup."""

from __future__ import annotations

from ...core.config.models import DirectTaskConfig, RslRlRunnerConfig, WorkerConfig
from ...core.config.robot import RobotSpec
from ...core.direct.curriculum import CurriculumManager
from ...core.direct.task import DirectTask
from ...errors import RegistryError
from ..evaluation import TaskEvaluator
from .models import TaskRegistration


class TaskRegistry:
    """Store explicitly registered Tasks without runtime module discovery.

    Registration is keyed by stable Task ID. A second registration is rejected
    even when the caller supplies a different configuration, so lookup remains
    deterministic for one process.
    """

    def __init__(self) -> None:
        """Create an empty explicit registry."""

        self._registrations: dict[str, TaskRegistration] = {}

    def register(self, registration: TaskRegistration) -> None:
        """Register one Task ID or raise a deterministic conflict error.

        Args:
            registration: Complete immutable Task metadata and per-Run factories.

        Raises:
            RegistryError: If the Task ID is empty, already exists at the same
                version, conflicts with an existing version, or contains a
                non-callable factory.

        Side effects:
            Add the registration to this registry only after all conflict checks
            pass. Failed registration leaves the registry unchanged.
        """

        if not registration.task_id:
            raise RegistryError("Task ID must not be empty", code="EMPTY_TASK_ID")
        for factory_name, factory in (
            ("task_factory", registration.task_factory),
            ("worker_config_factory", registration.worker_config_factory),
            ("task_config_factory", registration.task_config_factory),
            ("runner_config_factory", registration.runner_config_factory),
        ):
            if not callable(factory):
                raise RegistryError(
                    f"{factory_name} must be callable",
                    code="INVALID_FACTORY",
                    task_id=registration.task_id,
                )
        if registration.curriculum_factory is not None and not callable(registration.curriculum_factory):
            raise RegistryError(
                "curriculum_factory must be callable",
                code="INVALID_FACTORY",
                task_id=registration.task_id,
            )
        if registration.event_manager_factory is not None and not callable(
            registration.event_manager_factory
        ):
            raise RegistryError(
                "event_manager_factory must be callable",
                code="INVALID_FACTORY",
                task_id=registration.task_id,
            )
        for formatter_name, formatter in (
            ("evaluation_factory", registration.evaluation_factory),
            ("evaluation_formatter", registration.evaluation_formatter),
        ):
            if formatter is not None and not callable(formatter):
                raise RegistryError(
                    f"{formatter_name} must be callable",
                    code="INVALID_FACTORY",
                    task_id=registration.task_id,
                )
        existing = self._registrations.get(registration.task_id)
        if existing is not None:
            code = (
                "DUPLICATE_TASK_ID"
                if existing.task_version == registration.task_version
                else "TASK_VERSION_CONFLICT"
            )
            raise RegistryError(
                "Task ID is already registered",
                code=code,
                task_id=registration.task_id,
            )
        self._registrations[registration.task_id] = registration

    def resolve(self, task_id: str) -> TaskRegistration:
        """Return the unique registration for a Task ID.

        Args:
            task_id: Stable ID previously accepted by ``register``.

        Returns:
            The exact registered metadata object; the registry does not clone
            or mutate it.

        Raises:
            RegistryError: If no registration exists for ``task_id``.
        """

        try:
            return self._registrations[task_id]
        except KeyError as exc:
            raise RegistryError("Unknown Task ID", code="UNKNOWN_TASK_ID", task_id=task_id) from exc

    def create_task_config(self, task_id: str) -> DirectTaskConfig:
        """Create a fresh DirectTask Config from one registered Task.

        Args:
            task_id: Stable ID previously accepted by ``register``.

        Returns:
            A newly created typed Config owned by the caller's Run. The
            registration retains only the factory and never receives this value
            back for mutation.

        Raises:
            RegistryError: If no registration exists for ``task_id``.
        """

        return self.resolve(task_id).task_config_factory()

    def create_worker_config(self, task_id: str) -> WorkerConfig:
        """Create a fresh typed UE projection from one registered Task.

        Args:
            task_id: Stable ID previously accepted by ``register``.

        Returns:
            A newly created Worker Config for one Run.

        Raises:
            RegistryError: If no registration exists for ``task_id``.
        """

        return self.resolve(task_id).worker_config_factory()

    def create_runner_config(self, task_id: str) -> RslRlRunnerConfig:
        """Create a fresh Python runner Config from one registered Task.

        Args:
            task_id: Stable ID previously accepted by ``register``.

        Returns:
            A newly created runner Config retained by the Python Run only.

        Raises:
            RegistryError: If no registration exists for ``task_id``.
        """

        return self.resolve(task_id).runner_config_factory()

    def create_task(
        self,
        task_id: str,
        config: DirectTaskConfig | None = None,
        *,
        robot_spec: RobotSpec | None = None,
    ) -> DirectTask:
        """Create a fresh DirectTask from a registered Task and optional Config.

        Args:
            task_id: Stable ID previously accepted by ``register``.
            config: Typed DirectTask Config to pass to the factory. When omitted,
                the registry creates one fresh Config first.

        Returns:
            A new Task instance with no shared mutable registration state.

        Raises:
            RegistryError: If no registration exists for ``task_id``.
        """

        registration = self.resolve(task_id)
        task_config = config if config is not None else registration.task_config_factory()
        if robot_spec is None:
            return registration.task_factory(task_config)
        return registration.task_factory(task_config, robot_spec=robot_spec)

    def create_curriculum(
        self,
        task_id: str,
        config: DirectTaskConfig,
        *,
        num_envs: int,
        device: str,
        run_seed: int,
    ) -> CurriculumManager | None:
        """Create a task's optional curriculum manager for one Run."""

        factory = self.resolve(task_id).curriculum_factory
        if factory is None:
            return None
        return factory(config, num_envs, device, run_seed)

    def create_evaluator(self, task_id: str, *, sample_hz: float) -> TaskEvaluator | None:
        """Create the optional Task-owned evaluator for one rollout."""

        factory = self.resolve(task_id).evaluation_factory
        return None if factory is None else factory(sample_hz)

    def format_evaluation(self, task_id: str, result: object) -> str | None:
        """Format one result through its Task-owned formatter, if provided."""

        formatter = self.resolve(task_id).evaluation_formatter
        return None if formatter is None else formatter(result)

    def list(self) -> tuple[TaskRegistration, ...]:
        """Return registrations in stable Task ID and version order.

        Returns:
            A new tuple sorted by ``(task_id, task_version)``. Mutating the
            returned tuple is impossible and does not affect the registry.
        """

        return tuple(sorted(self._registrations.values(), key=lambda item: (item.task_id, item.task_version)))

"""Load explicitly enabled Task factories from installed Python distributions."""

from collections.abc import Sequence
from importlib import metadata

from ...errors import RegistryError
from .tasks import TaskRegistry


def register_external_tasks(registry: TaskRegistry, names: Sequence[str]) -> None:
    """Load one zero-argument TaskRegistration factory per entry-point name."""

    if not names:
        return
    points = metadata.entry_points(group="uerl.tasks")
    for name in names:
        matches = [point for point in points if point.name == name]
        if not matches:
            raise RegistryError(
                f"External Task entry point {name!r} not found in group 'uerl.tasks'; "
                "install its package or correct UERL_TASK_PLUGINS",
                code="EXTERNAL_TASK_NOT_FOUND",
            )
        if len(matches) != 1:
            raise RegistryError(
                f"External Task entry point {name!r} is declared by multiple installed packages; "
                "use unique entry-point names",
                code="DUPLICATE_ENTRY_POINT",
            )
        point = matches[0]
        try:
            factory = point.load()
            if not callable(factory):
                raise RegistryError("entry point must be a callable factory", code="INVALID_FACTORY")
            registration = factory()
            registry.register(registration)
        except RegistryError as exc:
            raise RegistryError(
                f"External Task {name!r} ({point.value}), Task ID {exc.task_id!r}: {exc}",
                code=exc.code,
                task_id=exc.task_id,
            ) from exc
        except Exception as exc:
            raise RegistryError(
                f"External Task {name!r} ({point.value}) failed to load: {exc}; "
                "check the installed package and its zero-argument registration factory",
                code="EXTERNAL_TASK_LOAD_FAILED",
            ) from exc

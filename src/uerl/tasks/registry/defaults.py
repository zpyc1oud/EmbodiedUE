"""Build the explicit product Task Registry used by Python entry points."""

from __future__ import annotations

import os
from collections.abc import Sequence

from .external import register_external_tasks
from .tasks import TaskRegistry


def create_default_registry(*, external_tasks: Sequence[str] | None = None) -> TaskRegistry:
    """Combine built-ins with enabled ``uerl.tasks`` entry-point factories.

    ``None`` reads comma-separated entry-point names from UERL_TASK_PLUGINS;
    an explicit sequence (including an empty one) overrides the environment.
    Enabled factories execute trusted Python code in this process.
    """

    from ...tasks.cartpole.registration import register_cartpole
    from ...tasks.phantomx.registration import register_phantomx

    registry = TaskRegistry()
    register_cartpole(registry)
    register_phantomx(registry)
    names = external_tasks
    if names is None:
        names = tuple(name.strip() for name in os.environ.get("UERL_TASK_PLUGINS", "").split(",") if name.strip())
    register_external_tasks(registry, names)
    return registry


__all__ = ["create_default_registry"]

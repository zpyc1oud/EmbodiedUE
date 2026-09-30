"""Build the explicit product Task Registry used by Python entry points."""

from __future__ import annotations

from .tasks import TaskRegistry


def create_default_registry() -> TaskRegistry:
    """Create a Registry containing the explicitly supported product Tasks."""

    from ...tasks.cartpole.registration import register_cartpole
    from ...tasks.phantomx.registration import register_phantomx

    registry = TaskRegistry()
    register_cartpole(registry)
    register_phantomx(registry)
    return registry


__all__ = ["create_default_registry"]

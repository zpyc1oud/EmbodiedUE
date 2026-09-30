"""Declarative MDP term configuration used by managers."""

from __future__ import annotations

from dataclasses import field
from typing import Any, Literal

from uerl.core.config.configspec import MISSING, configspec
from uerl.core.config.entity import RobotEntityCfg


@configspec
class DoneTermCfg:
    """One named termination term with optional assemble-time params."""

    func: Any = MISSING
    time_out: bool = False
    params: dict[str, Any] = field(default_factory=dict)


@configspec
class TerminationCfg:
    """Named termination terms consumed by ``TerminationManager``."""

    terms: dict[str, DoneTermCfg] = MISSING


@configspec
class RewTermCfg:
    """One named reward term with weight and assemble-time params."""

    func: Any = MISSING
    weight: float = MISSING
    params: dict[str, Any] = field(default_factory=dict)
    consumes_terminations: bool = False
    time_mode: Literal["transition", "rate"] = "transition"


@configspec
class RewardCfg:
    """Named reward terms consumed by ``RewardManager``."""

    terms: dict[str, RewTermCfg] = MISSING
    reference_dt_s: float | None = None


@configspec
class ActionTermCfg:
    """One named action term compiled into the shared action plan."""

    entity: RobotEntityCfg = MISSING
    target_mode: Literal["position", "effort"] = "position"
    clip: tuple[float, float] | None = (-1.0, 1.0)
    scale: float | tuple[float, ...] = 1.0
    use_default_offset: bool = True


@configspec
class ActionCfg:
    """Named action terms consumed by ``ActionManager``."""

    terms: dict[str, ActionTermCfg] = MISSING


@configspec
class ObsTermCfg:
    """One named observation term compiled into the shared observation plan.

    Convenience fields ``scale`` then ``clip`` expand to separate plan ops in
    that fixed order (scale into the target unit, then clip in that unit).
    """

    op: str = MISSING
    params: dict[str, Any] = field(default_factory=dict)
    inputs: tuple[str, ...] = ()
    scale: float | tuple[float, ...] = 1.0
    clip: tuple[float, float] | None = None


@configspec
class ObsGroupCfg:
    """Named observation terms that form one observation group.

    When ``members`` is omitted, every term name joins the group in declaration
    order. Set ``members`` to select a subset so intermediate slots can exist
    without entering the concatenated group vector.
    """

    terms: dict[str, ObsTermCfg] = MISSING
    concatenate: bool = True
    members: tuple[str, ...] | None = None


@configspec
class ObservationCfg:
    """Observation groups consumed by ``ObservationManager``."""

    groups: dict[str, ObsGroupCfg] = MISSING


@configspec
class CurrTermCfg:
    """One named curriculum term with assemble-time constructor params."""

    term_class: Any = MISSING
    params: dict[str, Any] = field(default_factory=dict)


@configspec
class CurriculumCfg:
    """Named curriculum terms consumed by ``CurriculumManager``."""

    terms: dict[str, CurrTermCfg] = MISSING


@configspec
class EventTermCfg:
    """One named event term with mode and optional interval range."""

    func: Any = MISSING
    mode: Literal["startup", "reset", "interval"] = MISSING
    interval_range_s: tuple[float, float] | None = None
    params: dict[str, Any] = field(default_factory=dict)


@configspec
class EventCfg:
    """Named event terms consumed by ``EventManager``."""

    terms: dict[str, EventTermCfg] = MISSING


__all__ = [
    "ActionCfg",
    "ActionTermCfg",
    "CurrTermCfg",
    "CurriculumCfg",
    "DoneTermCfg",
    "EventCfg",
    "EventTermCfg",
    "ObsGroupCfg",
    "ObsTermCfg",
    "ObservationCfg",
    "RewTermCfg",
    "RewardCfg",
    "TerminationCfg",
]

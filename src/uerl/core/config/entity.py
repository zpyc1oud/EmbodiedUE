"""Resolve robot joint/body subsets from names or regex patterns."""

from __future__ import annotations

import re
from collections.abc import Sequence
from dataclasses import field

from uerl.errors import ConfigError

from .configspec import configspec
from .robot import RobotSpec


def match_names(
    patterns: str | Sequence[str] | None,
    names: Sequence[str],
    *,
    preserve_order: bool,
    path: str,
) -> tuple[int, ...]:
    """Match ``patterns`` against ``names``.

    - A single regex string: hits in topology order.
    - A name/regex list with ``preserve_order=True``: hits per pattern in list order.
    - A name/regex list with ``preserve_order=False``: union of hits in topology order.
    - Any pattern with zero hits raises ``ConfigError`` listing available names.
    """

    if patterns is None:
        return ()
    available = ", ".join(names) if names else "<none>"
    if isinstance(patterns, str):
        regex = re.compile(patterns)
        hits = tuple(index for index, name in enumerate(names) if regex.fullmatch(name))
        if not hits:
            raise ConfigError(
                f"pattern {patterns!r} matched no names; available: {available}",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )
        return hits

    compiled = [(pattern, re.compile(pattern)) for pattern in patterns]
    if not compiled:
        raise ConfigError(
            f"pattern list is empty; available: {available}",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )

    if preserve_order:
        ordered: list[int] = []
        for pattern, regex in compiled:
            pattern_hits = [index for index, name in enumerate(names) if regex.fullmatch(name)]
            if not pattern_hits:
                raise ConfigError(
                    f"pattern {pattern!r} matched no names; available: {available}",
                    code="CONFIG_OUT_OF_RANGE",
                    path=path,
                )
            ordered.extend(pattern_hits)
        return tuple(ordered)

    selected: set[int] = set()
    for pattern, regex in compiled:
        hits_list = [index for index, name in enumerate(names) if regex.fullmatch(name)]
        if not hits_list:
            raise ConfigError(
                f"pattern {pattern!r} matched no names; available: {available}",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )
        selected.update(hits_list)
    return tuple(index for index in range(len(names)) if index in selected)


@configspec
class RobotEntityCfg:
    """Select robot joints and bodies by name or regex; bind at assemble time.

    Call ``resolve(spec)`` against a reflected ``RobotSpec`` before reading
    derived ids or plan field names. A pattern that matches nothing fails in
    ``resolve`` — empty selections are never returned silently.
    """

    joint_names: str | tuple[str, ...] | None = None
    body_names: str | tuple[str, ...] | None = None
    preserve_order: bool = False

    _joint_ids: tuple[int, ...] = field(default=(), repr=False)
    _body_ids: tuple[int, ...] = field(default=(), repr=False)
    _joint_field_names: tuple[str, ...] = field(default=(), repr=False)
    _body_field_names: tuple[str, ...] = field(default=(), repr=False)
    _joint_lower_limits: tuple[float | None, ...] = field(default=(), repr=False)
    _joint_upper_limits: tuple[float | None, ...] = field(default=(), repr=False)
    _resolved: bool = field(default=False, repr=False)

    @property
    def resolved(self) -> bool:
        return self._resolved

    @property
    def joint_ids(self) -> tuple[int, ...]:
        self._require_resolved()
        return self._joint_ids

    @property
    def body_ids(self) -> tuple[int, ...]:
        self._require_resolved()
        return self._body_ids

    @property
    def joint_field_names(self) -> tuple[str, ...]:
        self._require_resolved()
        return self._joint_field_names

    @property
    def body_field_names(self) -> tuple[str, ...]:
        self._require_resolved()
        return self._body_field_names

    @property
    def joint_lower_limits(self) -> tuple[float | None, ...]:
        self._require_resolved()
        return self._joint_lower_limits

    @property
    def joint_upper_limits(self) -> tuple[float | None, ...]:
        self._require_resolved()
        return self._joint_upper_limits

    def resolve(self, spec: RobotSpec) -> None:
        """Bind name/regex selectors to topology indices and plan field names."""

        joint_names = tuple(joint.name for joint in spec.topology.joints)
        body_names = tuple(spec.body_names)
        joint_ids = match_names(
            self.joint_names,
            joint_names,
            preserve_order=self.preserve_order,
            path="joint_names",
        )
        body_ids = match_names(
            self.body_names,
            body_names,
            preserve_order=self.preserve_order,
            path="body_names",
        )
        joint_field_names = tuple(f"robot.joint.{joint_names[index]}" for index in joint_ids)
        body_field_names = tuple(f"robot.body.{body_names[index]}" for index in body_ids)
        joint_lower_limits = tuple(spec.topology.joints[index].lower_limit for index in joint_ids)
        joint_upper_limits = tuple(spec.topology.joints[index].upper_limit for index in joint_ids)

        if self._resolved:
            if (
                joint_ids != self._joint_ids
                or body_ids != self._body_ids
                or joint_field_names != self._joint_field_names
                or body_field_names != self._body_field_names
                or joint_lower_limits != self._joint_lower_limits
                or joint_upper_limits != self._joint_upper_limits
            ):
                raise ConfigError(
                    "RobotEntityCfg.resolve is not idempotent for this spec",
                    code="CONFIG_OUT_OF_RANGE",
                    path="resolved",
                )
            return

        object.__setattr__(self, "_joint_ids", joint_ids)
        object.__setattr__(self, "_body_ids", body_ids)
        object.__setattr__(self, "_joint_field_names", joint_field_names)
        object.__setattr__(self, "_body_field_names", body_field_names)
        object.__setattr__(self, "_joint_lower_limits", joint_lower_limits)
        object.__setattr__(self, "_joint_upper_limits", joint_upper_limits)
        object.__setattr__(self, "_resolved", True)

    def _require_resolved(self) -> None:
        if not self._resolved:
            raise ConfigError(
                "RobotEntityCfg must be resolve()'d before reading derived fields",
                code="CONFIG_OUT_OF_RANGE",
                path="resolved",
            )


__all__ = [
    "RobotEntityCfg",
    "match_names",
]

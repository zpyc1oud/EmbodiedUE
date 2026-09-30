"""Verify ticket35 scene-randomization terms stay on the Session path."""

from __future__ import annotations

from types import SimpleNamespace
from typing import Any, cast

import pytest
import torch

from uerl import UERLSessionAdapter
from uerl.core.config.robot import RobotSpec, RobotTopology
from uerl.core.mdp.lib.events import (
    EventEffect,
    RecordingEventBridge,
    push_root,
    randomize_ground_friction,
    randomize_terrain_tier_params,
)
from uerl.core.mdp.managers.event import EventManager
from uerl.core.mdp.terms import EventCfg, EventTermCfg
from uerl.errors import ConfigError
from uerl.tasks.phantomx.generalization import (
    GENERALIZATION_EVENT_NAMES,
    create_generalization_event_manager,
    select_generalization_event_names,
)


def _robot_spec() -> RobotSpec:
    """Return the smallest topology accepted by EventManager preparation."""

    topology = RobotTopology(
        body_names=("root",),
        body_motion_types=("simulated",),
        root_body_index=0,
        fixed_base=False,
        joints=(),
    )
    return RobotSpec(actuators=(), observations=(), reset=(), body_names=("root",), topology=topology)


def _generator(seed: int) -> torch.Generator:
    generator = torch.Generator(device="cpu")
    generator.manual_seed(seed)
    return generator


def _sample_effects(seed: int) -> tuple[EventEffect, ...]:
    """Run all three production terms through the ticket30 EventManager."""

    spec = _robot_spec()
    cfg = EventCfg(
        terms={
            "friction": EventTermCfg(
                func=randomize_ground_friction,
                mode="startup",
                params={
                    "static_range": (0.7, 1.1),
                    "dynamic_range": (0.5, 0.9),
                },
            ),
            "terrain": EventTermCfg(
                func=randomize_terrain_tier_params,
                mode="startup",
                params={"roughness_scale_range": (0.2, 0.8)},
            ),
            "push": EventTermCfg(
                func=push_root,
                mode="startup",
                params={"velocity_range_mps": (-0.3, 0.3)},
            ),
        }
    )
    manager = EventManager(
        cfg,
        spec,
        batch_size=4,
        device="cpu",
        generator=_generator(seed),
    )
    bridge = RecordingEventBridge()
    manager.bind_bridge(bridge)
    manager.apply("startup", mask=torch.tensor([True, False, True, False]))
    return bridge.drain_effects()


def test_three_scene_terms_sample_declared_ranges_and_reproduce() -> None:
    """Each term samples selected rows, preserves masks, and is seed-stable."""

    first = _sample_effects(17)
    second = _sample_effects(17)
    different = _sample_effects(18)

    assert [effect.kind for effect in first] == [
        "ground_friction",
        "terrain_tier_params",
        "root_push",
    ]
    assert len(first) == len(second) == len(different) == 3
    for left, right in zip(first, second, strict=True):
        assert torch.equal(left.mask, right.mask)
        assert torch.equal(left.values, right.values)
    assert any(not torch.equal(left.values, right.values) for left, right in zip(first, different, strict=True))

    selected = torch.tensor([True, False, True, False])
    for effect in first:
        assert torch.equal(effect.mask, selected)
        assert torch.equal(effect.values[~selected], torch.zeros_like(effect.values[~selected]))
    friction = first[0].values
    assert bool(((friction[selected, 0] >= 0.7) & (friction[selected, 0] <= 1.1)).all())
    assert bool(((friction[selected, 1] >= 0.5) & (friction[selected, 1] <= 0.9)).all())
    roughness = first[1].values[:, 0]
    assert bool(((roughness[selected] >= 0.2) & (roughness[selected] <= 0.8)).all())
    push = first[2].values
    assert bool(((push[selected] >= -0.3) & (push[selected] <= 0.3)).all())


class _RawEventSession:
    """Expose only the raw event surface needed by the typed adapter test."""

    def __init__(self) -> None:
        self.config: Any = SimpleNamespace(worker=SimpleNamespace(slot_count=4, terrain_config={}))
        self.requests: list[dict[str, Any]] = []

    def event(self, request: dict[str, Any]) -> dict[str, Any]:
        self.requests.append(request)
        return {"applied": True, "kind": request["kind"]}


def test_event_effect_is_forwarded_as_selected_slot_session_request() -> None:
    """The typed adapter sends only selected rows to the raw Session boundary."""

    raw = _RawEventSession()
    adapter = UERLSessionAdapter(cast(Any, raw))
    adapter.apply_event(
        EventEffect(
            "root_push",
            torch.tensor(
                [[0.0, 0.0, 0.0], [0.25, 0.5, 0.75], [1.0, 1.25, 1.5], [0.0, 0.0, 0.0]],
                dtype=torch.float32,
            ),
            torch.tensor([False, True, True, False]),
        )
    )

    assert raw.requests == [
        {
            "kind": "root_push",
            "slot_ids": [1, 2],
            "values": [[0.25, 0.5, 0.75], [1.0, 1.25, 1.5]],
        }
    ]


def test_generalization_event_ablation_preserves_stable_term_order() -> None:
    """The tool can isolate terms without changing their production order."""

    enabled = select_generalization_event_names(("terrain_tier_params",))
    assert enabled == ("ground_friction", "root_push")

    manager = create_generalization_event_manager(
        _robot_spec(),
        batch_size=4,
        device="cpu",
        run_seed=17,
        enabled_terms=enabled,
    )
    bridge = RecordingEventBridge()
    manager.bind_bridge(bridge)
    manager.apply("startup", mask=torch.ones(4, dtype=torch.bool))
    startup = bridge.drain_effects()
    assert [effect.kind for effect in startup] == ["ground_friction"]
    manager.apply("interval", dt=2.0)
    interval = bridge.drain_effects()
    assert [effect.kind for effect in interval] == ["root_push"]


def test_generalization_event_ablation_rejects_unknown_or_empty_selection() -> None:
    """An ablation typo must fail before training instead of silently changing it."""

    with pytest.raises(ConfigError, match="unknown generalization event terms"):
        select_generalization_event_names(("not-an-event",))
    assert select_generalization_event_names(GENERALIZATION_EVENT_NAMES) == ()
    with pytest.raises(ConfigError, match="at least one generalization event term"):
        create_generalization_event_manager(
            _robot_spec(),
            batch_size=4,
            device="cpu",
            run_seed=17,
            enabled_terms=(),
        )

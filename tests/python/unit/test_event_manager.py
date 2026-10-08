"""Ticket 30: EventManager mode scheduling, reset parity, and Session-only effects."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import torch

from uerl.core.config.robot import RobotTopology, merge_robot_spec, parse_robot_config
from uerl.core.mdp.lib.events import (
    RecordingEventBridge,
    sample_robot_reset_distributions,
    sample_robot_reset_event,
)
from uerl.core.mdp.managers.event import EventManager
from uerl.core.mdp.terms import EventCfg, EventTermCfg

_CARTPOLE_YAML = """
actuators:
  - joint: cart
    stiffness: 0.0
    damping: 1.0
    effort_limit: 100.0
    default_pos: 0.0
    action_scale: 100.0
observations:
  - type: joint_position
    joint: pole
reset:
  distributions:
    - type: joint_position
      joint: pole
      distribution:
        type: uniform
        lower: -0.05
        upper: 0.05
        stream_id: reset.pole
"""


def _seeded_generator(seed: int) -> torch.Generator:
    generator = torch.Generator(device="cpu")
    generator.manual_seed(seed)
    return generator


def _cartpole_bundle() -> tuple[Any, Any]:
    from uerl.core.config.robot import ConstraintFrame, JointTopology

    frame = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))

    def _joint(
        name: str,
        parent: int,
        child: int,
        coordinate: str,
        coordinate_type: str,
        unit: str,
        lower: float | None,
        upper: float | None,
        *,
        default_position: float = 0.0,
    ) -> JointTopology:
        return JointTopology(
            name,
            parent,
            child,
            1,
            coordinate,
            coordinate_type,
            unit,
            lower,
            upper,
            frame,
            frame,
            default_position,
        )

    topology = RobotTopology(
        body_names=("base", "cart", "pole"),
        body_motion_types=("kinematic", "simulated", "simulated"),
        root_body_index=0,
        fixed_base=True,
        joints=(
            _joint("cart", 0, 1, "linear_x", "prismatic", "m", None, None),
            _joint("pole", 1, 2, "twist", "revolute", "rad", -3.14, 3.14),
        ),
    )
    semantics = parse_robot_config(_CARTPOLE_YAML)
    spec = merge_robot_spec(semantics, topology)
    return semantics, spec


def _record_call(
    bridge: RecordingEventBridge,
    mask: torch.Tensor,
    *,
    value: float,
    generator: torch.Generator | None = None,
    **_params: object,
) -> None:
    del generator
    # Stage a sentinel tensor so AC_006 can prove Session-bridge-only mutation.
    bridge.write_robot_reset_values(torch.full((mask.numel(), 1), value), mask)


def test_ac_py_unit_evtmgr_001_modes_fire_at_correct_timing() -> None:
    """AC_PY_UNIT_EVTMGR_001: each mode fires on apply; available_modes filters empty."""

    fired: list[str] = []

    def _startup(bridge: RecordingEventBridge, mask: torch.Tensor, **kwargs: object) -> None:
        del bridge, mask, kwargs
        fired.append("startup")

    def _reset(bridge: RecordingEventBridge, mask: torch.Tensor, **kwargs: object) -> None:
        del bridge, mask, kwargs
        fired.append("reset")

    def _interval(bridge: RecordingEventBridge, mask: torch.Tensor, **kwargs: object) -> None:
        del bridge, kwargs
        fired.append(f"interval:{int(mask.sum().item())}")

    cfg = EventCfg(
        terms={
            "boot": EventTermCfg(func=_startup, mode="startup"),
            "reseed": EventTermCfg(func=_reset, mode="reset"),
            "nudge": EventTermCfg(
                func=_interval,
                mode="interval",
                interval_range_s=(0.05, 0.05),
            ),
        }
    )
    _, spec = _cartpole_bundle()
    manager = EventManager(cfg, spec, batch_size=4, device="cpu", generator=_seeded_generator(7))
    bridge = RecordingEventBridge()
    manager.bind_bridge(bridge)

    assert manager.available_modes == frozenset({"startup", "reset", "interval"})
    manager.apply("startup")
    manager.apply("reset", mask=torch.tensor([True, False, True, False]))
    # First interval countdown is 0.05s; one 0.05 step fires all slots.
    due = manager.apply("interval", dt=0.05)
    assert fired == ["startup", "reset", "interval:4"]
    assert due is not None and bool(due.all())


def test_ac_py_unit_evtmgr_002_reset_sampling_bit_identical() -> None:
    """AC_PY_UNIT_EVTMGR_002: event reset path matches direct sample bit-for-bit."""

    semantics, spec = _cartpole_bundle()
    run_seed = 42
    episode_index = torch.tensor([0, 3, 1, 8], dtype=torch.uint64)
    mask = torch.tensor([True, True, False, True])

    direct = sample_robot_reset_distributions(
        robot_spec=spec,
        robot_semantics=semantics,
        run_seed=run_seed,
        episode_index=episode_index,
        reset_mask=mask,
    )
    assert direct is not None

    cfg = EventCfg(
        terms={
            "reset_pose": EventTermCfg(
                func=sample_robot_reset_event,
                mode="reset",
                params={
                    "robot_spec": spec,
                    "robot_semantics": semantics,
                    "run_seed": run_seed,
                    "episode_index": episode_index,
                },
            )
        }
    )
    manager = EventManager(cfg, spec, batch_size=4, device="cpu", generator=_seeded_generator(0))
    bridge = RecordingEventBridge()
    manager.bind_bridge(bridge)
    manager.apply("reset", mask=mask)

    assert bridge.robot_reset_values is not None
    assert torch.equal(bridge.robot_reset_values, direct)
    # Unselected rows stay zero in both paths.
    assert torch.equal(direct[2], torch.zeros_like(direct[2]))


def test_ac_py_unit_evtmgr_003_interval_timers_are_slot_independent() -> None:
    """AC_PY_UNIT_EVTMGR_003: interval countdowns differ across ≥4 Slots."""

    def _noop(bridge: RecordingEventBridge, mask: torch.Tensor, **kwargs: object) -> None:
        del bridge, mask, kwargs

    cfg = EventCfg(
        terms={
            "push": EventTermCfg(
                func=_noop,
                mode="interval",
                interval_range_s=(0.1, 1.0),
            )
        }
    )
    _, spec = _cartpole_bundle()
    manager = EventManager(cfg, spec, batch_size=4, device="cpu", generator=_seeded_generator(99))
    manager.bind_bridge(RecordingEventBridge())
    due = manager.apply("interval", dt=0.5)
    assert due is not None
    assert 0 < int(due.sum()) < 4


def test_ac_py_unit_evtmgr_004_seeded_sequences_reproduce() -> None:
    """AC_PY_UNIT_EVTMGR_004: same seed → same interval fire sequence."""

    def _mark(bridge: RecordingEventBridge, mask: torch.Tensor, **kwargs: object) -> None:
        del kwargs
        bridge.write_terrain_levels(mask.to(dtype=torch.uint16), mask)

    cfg = EventCfg(
        terms={
            "tick": EventTermCfg(
                func=_mark,
                mode="interval",
                interval_range_s=(0.1, 0.4),
            )
        }
    )
    _, spec = _cartpole_bundle()

    def _run(seed: int) -> list[list[bool]]:
        manager = EventManager(cfg, spec, batch_size=4, device="cpu", generator=_seeded_generator(seed))
        bridge = RecordingEventBridge()
        manager.bind_bridge(bridge)
        sequence: list[list[bool]] = []
        for _ in range(12):
            due = manager.apply("interval", dt=0.05)
            assert due is not None
            sequence.append([bool(v) for v in due.tolist()])
        return sequence

    assert _run(123) == _run(123)
    assert _run(123) != _run(456)


def test_ac_py_unit_evtmgr_005_reset_mask_selects_rows() -> None:
    """AC_PY_UNIT_EVTMGR_005: reset apply only touches selected Slots."""

    seen_masks: list[torch.Tensor] = []

    def _capture(bridge: RecordingEventBridge, mask: torch.Tensor, **kwargs: object) -> None:
        del bridge, kwargs
        seen_masks.append(mask.clone())

    cfg = EventCfg(terms={"reset": EventTermCfg(func=_capture, mode="reset")})
    _, spec = _cartpole_bundle()
    manager = EventManager(cfg, spec, batch_size=4, device="cpu", generator=_seeded_generator(1))
    manager.bind_bridge(RecordingEventBridge())
    mask = torch.tensor([False, True, False, True])
    manager.apply("reset", mask=mask)
    assert len(seen_masks) == 1
    assert torch.equal(seen_masks[0], mask)


def test_ac_py_unit_evtmgr_006_events_only_use_session_bridge() -> None:
    """AC_PY_UNIT_EVTMGR_006: mutations appear only as Session bridge calls."""

    cfg = EventCfg(
        terms={
            "startup": EventTermCfg(func=_record_call, mode="startup", params={"value": 11.0}),
            "reset": EventTermCfg(func=_record_call, mode="reset", params={"value": 23.0}),
        }
    )
    _, spec = _cartpole_bundle()
    manager = EventManager(cfg, spec, batch_size=2, device="cpu", generator=_seeded_generator(3))
    bridge = RecordingEventBridge()
    manager.bind_bridge(bridge)
    manager.apply("startup")
    manager.apply("reset", mask=torch.tensor([True, False]))

    assert [call[0] for call in bridge.calls] == [
        "write_robot_reset_values",
        "write_robot_reset_values",
    ]
    assert bridge.robot_reset_values is not None
    assert torch.equal(bridge.calls[0][1], torch.tensor([[11.0], [11.0]]))
    assert torch.equal(bridge.calls[0][2], torch.tensor([True, True]))
    assert torch.equal(bridge.calls[1][1], torch.tensor([[23.0], [23.0]]))
    assert torch.equal(bridge.calls[1][2], torch.tensor([True, False]))
    assert torch.equal(bridge.robot_reset_values, torch.tensor([[23.0], [11.0]]))


def test_available_modes_omits_empty_modes() -> None:
    cfg = EventCfg(terms={"only_reset": EventTermCfg(func=_record_call, mode="reset", params={"value": 7.0})})
    _, spec = _cartpole_bundle()
    manager = EventManager(cfg, spec, batch_size=1, device="cpu", generator=_seeded_generator(0))
    assert manager.available_modes == frozenset({"reset"})


def test_ac_py_unit_evtmgr_007_interval_terms_follow_their_own_ranges() -> None:
    """A fast push cannot pull a slower configured event forward."""

    fired: list[tuple[str, list[bool]]] = []

    def capture(name: str) -> Callable[..., None]:
        def term(_bridge: RecordingEventBridge, mask: torch.Tensor, **_kwargs: object) -> None:
            fired.append((name, mask.tolist()))

        return term

    _, spec = _cartpole_bundle()
    manager = EventManager(
        EventCfg(terms={
            "fast": EventTermCfg(func=capture("fast"), mode="interval", interval_range_s=(0.1, 0.1)),
            "slow": EventTermCfg(func=capture("slow"), mode="interval", interval_range_s=(0.3, 0.3)),
        }),
        spec, batch_size=2, device="cpu", generator=_seeded_generator(2),
    )
    manager.bind_bridge(RecordingEventBridge())
    manager.apply("interval", dt=0.1)
    assert fired == [("fast", [True, True])]
    manager.apply("interval", dt=0.1)
    assert fired == [("fast", [True, True]), ("fast", [True, True])]
    manager.apply("interval", dt=0.101)
    assert fired[-2:] == [("fast", [True, True]), ("slow", [True, True])]


def test_ac_py_unit_evtmgr_008_reset_restarts_only_selected_slot_interval() -> None:
    """New episodes receive a full interval while survivors retain their countdown."""

    fired: list[list[bool]] = []

    def capture(_bridge: RecordingEventBridge, mask: torch.Tensor, **_kwargs: object) -> None:
        fired.append(mask.tolist())

    _, spec = _cartpole_bundle()
    manager = EventManager(
        EventCfg(terms={"push": EventTermCfg(func=capture, mode="interval", interval_range_s=(0.2, 0.2))}),
        spec, batch_size=2, device="cpu", generator=_seeded_generator(2),
    )
    manager.bind_bridge(RecordingEventBridge())
    manager.apply("interval", dt=0.1)
    manager.reset(torch.tensor([True, False]))
    manager.apply("interval", dt=0.1)
    assert fired == [[False, True]]

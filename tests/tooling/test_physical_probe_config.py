"""Resolve real-process physical fixtures without launching an engine."""
import struct
from pathlib import Path

import pytest

from tests.e2e.test_phantomx_physical_response import _config, _wire_state
from uerl.core.codec.batch import Layout, Segment


@pytest.mark.parametrize("slots", [1, 2])
@pytest.mark.parametrize("decimation", [1, 4])
@pytest.mark.parametrize("generated", [False, True])
def test_physical_probe_resolves_a_supported_ground_and_fixed_clock(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, slots: int, decimation: int, generated: bool,
) -> None:
    # Configuration-only check: reserve no socket and launch no process.
    monkeypatch.setattr("uerl.training.configuration._free_port", lambda: 19001)
    config = _config(tmp_path / "run", slots, decimation, generated_flat=generated)
    assert config.worker.slot_count == slots
    assert config.worker.decimation == (decimation, decimation)
    assert config.worker.physics_dt == pytest.approx(0.005)
    assert config.session.map_path == ("/Engine/Maps/Entry" if generated else "/Game/Maps/NewMap")
    if generated:
        assert config.worker.terrain_config["num_levels"] == 1
        assert config.worker.terrain_config["physics_collision"] is True
        assert config.worker.terrain_config["tiers"][0]["primitive"] == "plane"  # type: ignore[index]
    else:
        assert not config.worker.terrain_config


@pytest.mark.parametrize("valid,fault", [(1, 0), (0, 0), (1, 3)])
def test_physical_wire_capture_rejects_invalid_or_faulted_rows(valid: int, fault: int) -> None:
    # This tests the capture's acceptance logic, not layout negotiation or UE.
    layout = Layout("step_result", 24, 0, "unused-by-capture", (
        Segment("system.state_valid", "uint8", (2,), 0, 2),
        Segment("system.slot_fault_code", "uint16", (2,), 8, 4),
        Segment("robot.joint.test.joint_position", "float32", (2, 1), 16, 8),
    ))
    payload = bytearray(24)
    struct.pack_into("<2B", payload, 0, 1, valid)
    struct.pack_into("<2H", payload, 8, 0, fault)
    struct.pack_into("<2f", payload, 16, 0.25, -0.5)
    names = ("robot.joint.test.joint_position",)
    if valid == 1 and fault == 0:
        assert _wire_state(bytes(payload), layout, names, 2)[names[0]].tolist() == [[0.25], [-0.5]]
    else:
        with pytest.raises(AssertionError):
            _wire_state(bytes(payload), layout, names, 2)

"""Resolve real-process physical fixtures without launching an engine."""
from pathlib import Path

import pytest

from tests.e2e.test_phantomx_physical_response import _config


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

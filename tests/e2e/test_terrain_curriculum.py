"""Exercise the terrain INITIALIZE and RESET seam against a real UE Worker."""

from __future__ import annotations

import socket
from collections.abc import Sequence
from dataclasses import replace
from pathlib import Path
from typing import Any

import numpy as np
import pytest

from tests.e2e.support.worker_runner import UE_CMD, UPROJECT
from uerl import (
    ObservationShapeTable,
    ResolvedRunConfig,
    SessionSchema,
    UERLSession,
    robot_actuator_action_schema,
    robot_observation_schema,
)
from uerl.core.codec import BridgeProtocolError, Layout, sha256_json
from uerl.core.config.canonical import canonical_json, sha256_hex, to_jsonable
from uerl.core.config.yaml_loader import load_unique_yaml
from uerl.core.direct.curriculum import TerrainCurriculum
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.phantomx import PHANTOMX_TERRAIN_TASK_ID
from uerl.training import build_launch_overrides, build_run_config

TRAIN_MAP = "/Engine/Maps/Entry"


def _free_port() -> int:
    """Reserve one local TCP port for the terrain Worker."""

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _resolved_config(
    port: int,
    run_directory: Path,
    terrain_config: dict[str, object],
) -> ResolvedRunConfig:
    """Build a two-Slot formal Session Config for the terrain Worker."""

    overrides = build_launch_overrides(
        ue_executable=Path(UE_CMD),
        project=Path(UPROJECT),
        map_name=TRAIN_MAP,
        port=port,
    )
    overrides.update({"worker.slot_count": "2", "logging.run_directory": str(run_directory)})
    base = build_run_config(PHANTOMX_TERRAIN_TASK_ID, overrides=overrides)
    config = replace(base, worker=replace(base.worker, terrain_config=terrain_config))
    payload = to_jsonable(config)
    payload.pop("normalized_hash")
    return replace(config, normalized_hash=sha256_hex(canonical_json(payload)))


def _terrain_config() -> dict[str, object]:
    """Return one compact config containing every supported UE primitive."""

    return {
        "num_levels": 3,
        "cell_size": [4.0, 4.0],
        "border_width": 0.5,
        "physics_collision": True,
        "tiers": [
            {"level": 0, "primitive": "plane", "seed": "11", "platform_width": 1.0, "params": {}},
            {
                "level": 1,
                "primitive": "heightfield",
                "seed": "22",
                "platform_width": 1.0,
                "params": {
                    "noise_range": [0.01, 0.06],
                    "noise_step": 0.01,
                    "horizontal_scale": 0.5,
                    "vertical_scale": 0.005,
                    "downsampled_scale": None,
                },
            },
            {
                "level": 2,
                "primitive": "boxes",
                "seed": "33",
                "platform_width": 1.0,
                "params": {"grid_width": 0.5, "grid_height_range": [0.025, 0.1], "holes": False},
            },
        ],
    }


def _reset_payload(
    layout: Layout,
    mask: tuple[bool, ...],
    levels: tuple[int, ...],
    reset_values: Sequence[float],
) -> bytes:
    """Encode one full-batch reset mask, terrain levels, and valid Robot values."""

    payload = bytearray(layout.payload_length)
    mask_segment = layout.segment("system.reset_mask")
    level_segment = layout.segment("terrain_level")
    assert mask_segment.shape == (1,)
    assert level_segment.dtype == "uint16"
    assert level_segment.shape == (len(levels),)
    assert level_segment.offset % 8 == 0
    for slot, selected in enumerate(mask):
        if selected:
            payload[mask_segment.offset + slot // 8] |= 1 << (slot % 8)
    raw_levels = np.asarray(levels, dtype="<u2").tobytes(order="C")
    payload[level_segment.offset : level_segment.offset + level_segment.byte_length] = raw_levels
    reset_segment = layout.segment("robot.reset.values")
    assert reset_segment.shape == (len(mask), len(reset_values))
    raw_reset_values = np.tile(np.asarray(reset_values, dtype="<f4"), len(mask)).tobytes(order="C")
    payload[reset_segment.offset : reset_segment.offset + reset_segment.byte_length] = raw_reset_values
    return bytes(payload)


def test_real_ue_negotiates_all_terrain_tiers_and_resets_each_slot(tmp_path: Path) -> None:
    """Verify all primitives are generated at INIT and levels travel on RESET."""

    assert Path(UE_CMD).exists()
    assert Path(UPROJECT).exists()
    terrain = _terrain_config()
    config = _resolved_config(_free_port(), tmp_path / "terrain-run", terrain)
    session = UERLSession.open(config, process_controller=WorkerProcessController())
    try:
        session.describe(SessionSchema((), ()).as_request())
        robot_spec = session.described_robot_spec
        assert robot_spec is not None
        observation_shapes = ObservationShapeTable.from_descriptor(session.descriptor)
        schema = SessionSchema(
            robot_observation_schema(robot_spec, observation_shapes),
            robot_actuator_action_schema(robot_spec),
        )
        result = session.initialize(schema.as_request())
        reset_values = tuple(target.reference + target.lower for target in robot_spec.reset)
        layouts = {
            item["layout_kind"]: Layout.parse(item, batch_size=config.worker.slot_count)
            for item in result.response["layouts"]
        }
        assert set(layouts) == {"initial_state", "step_action", "step_result", "reset_request", "reset_result"}
        reset_layout = layouts["reset_request"]
        assert reset_layout.payload_length > 12
        assert [segment.name for segment in reset_layout.segments] == [
            "system.reset_mask",
            "terrain_level",
            "robot.reset.values",
        ]
        assert reset_layout.segment("terrain_level").byte_length == 4

        effective_config: dict[str, Any] = result.response["effective_worker_config"]
        assert effective_config["environment"]["terrain"] == terrain
        assert result.response["effective_worker_config_hash"] == sha256_json(effective_config)
        manifest_path = config.logging.run_directory / "manifest.yaml"
        manifest_value = load_unique_yaml(manifest_path.read_text(encoding="utf-8"))
        assert isinstance(manifest_value, dict)
        manifest: dict[str, Any] = manifest_value
        resolved_config = manifest.get("resolved_config")
        assert isinstance(resolved_config, dict)
        assert resolved_config["normalized_hash"] == config.normalized_hash
        assert manifest["effective_worker_config"] == effective_config
        assert manifest["layout_hashes"]["reset_request"] == reset_layout.layout_hash
        identity = dict(manifest)
        identity.pop("run_directory")
        assert session.manifest_hash == sha256_hex(canonical_json(identity))

        session.acknowledge_ready()
        curriculum = TerrainCurriculum(num_levels=3, num_envs=2, terrain_size_x=2.0)
        assert curriculum.levels == (0, 0)

        _, first_reset = session.reset(
            reset_layout.layout_id,
            _reset_payload(reset_layout, (True, True), (0, 1), reset_values),
        )
        result_layout = layouts["reset_result"]

        def field(payload: bytes, name: str) -> np.ndarray[Any, np.dtype[np.float32]]:
            segment = result_layout.segment(name)
            return np.frombuffer(payload, dtype="<f4", count=segment.byte_length // 4,
                                 offset=segment.offset).reshape(2, -1)

        scan_name = "robot.body.base_link.terrain_height"
        first_scan = field(first_reset, scan_name)
        np.testing.assert_allclose(first_scan[0], first_scan[0, 0], atol=1e-6, rtol=0)
        assert np.ptp(first_scan[1]) > 1e-4, "heightfield Slot must observe non-flat ground"


        assert curriculum.update([0], [1.1], [2.0]) == (1, 0)
        _, second_reset = session.reset(
            reset_layout.layout_id,
            _reset_payload(reset_layout, (True, True), curriculum.levels, reset_values),
        )
        second_scan = field(second_reset, scan_name)
        assert np.ptp(second_scan[0]) > 1e-4, "Slot0 must switch from plane to heightfield"
        np.testing.assert_allclose(second_scan[1], second_scan[1, 0], atol=1e-6, rtol=0)


        assert curriculum.update([1], [1.1], [2.0]) == (1, 1)
        _, third_reset = session.reset(
            reset_layout.layout_id,
            _reset_payload(reset_layout, (False, True), curriculum.levels, reset_values),
        )
        assert np.ptp(field(third_reset, scan_name)[1]) > 1e-4, "selected Slot1 must switch tier"
        for descriptor in schema.state_requirements:
            name = str(descriptor["name"])
            np.testing.assert_array_equal(field(third_reset, name)[0], field(second_reset, name)[0],
                                          err_msg=f"unselected Slot0 {name}")
        _, boxes_reset = session.reset(
            reset_layout.layout_id,
            _reset_payload(reset_layout, (True, False), (2, 1), reset_values),
        )
        assert np.ptp(field(boxes_reset, scan_name)[0]) > 1e-4, "boxes tier must expose varying heights"
        for descriptor in schema.state_requirements:
            name = str(descriptor["name"])
            np.testing.assert_array_equal(field(boxes_reset, name)[1], field(third_reset, name)[1],
                                          err_msg=f"unselected Slot1 {name}")


        with pytest.raises(BridgeProtocolError):
            session.reset(
                reset_layout.layout_id,
                _reset_payload(reset_layout, (True, False), (3, 1), reset_values),
            )
    finally:
        session.close("terrain_e2e")

    print("[VERIFY] TERRAIN-E2E: init=all-primitives reset=per-slot curriculum=python-owned")

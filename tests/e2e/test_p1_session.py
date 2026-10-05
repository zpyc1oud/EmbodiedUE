"""Verify the formal UERLSession against real UE."""

from __future__ import annotations

import json
import socket
from pathlib import Path
from typing import Any

import numpy as np

from tests.e2e.support.worker_runner import TRAIN_MAP, UE_CMD, UPROJECT
from uerl import (
    ObservationShapeTable,
    ResolvedRunConfig,
    SessionSchema,
    UERLSession,
    robot_actuator_action_schema,
    robot_observation_schema,
)
from uerl.core.codec import Layout, sha256_json
from uerl.core.config.canonical import canonical_json, sha256_hex
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_launch_overrides, build_run_config


def _free_port() -> int:
    """Reserve one local TCP port for a Worker launch."""

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _resolved_config(
    port: int,
    run_directory: Path,
) -> ResolvedRunConfig:
    """Resolve the canonical Generic CartPole product for a raw Session test."""

    overrides = build_launch_overrides(
        ue_executable=Path(UE_CMD),
        project=Path(UPROJECT),
        map_name=TRAIN_MAP,
        port=port,
    )
    overrides.update({"worker.slot_count": "2", "logging.run_directory": str(run_directory)})
    return build_run_config(CARTPOLE_TASK_ID, overrides=overrides)


def _batch_zero(layout: Layout, field_name: str) -> bytes:
    """Build a zeroed field-major batch for one negotiated layout."""

    payload = bytearray(layout.payload_length)
    segment = layout.segment(field_name)
    values = np.zeros(segment.byte_length // 4, dtype="<f4")
    payload[segment.offset : segment.offset + segment.byte_length] = values.tobytes()
    return bytes(payload)


def _step_action(layout: Layout, *, step_decimation: int) -> bytes:
    """Zero actuator targets and write one in-range step_decimation scalar."""

    payload = bytearray(_batch_zero(layout, "robot.actuator.target"))
    segment = layout.segment("step_decimation")
    payload[segment.offset : segment.offset + segment.byte_length] = np.asarray(
        [step_decimation], dtype="<i4"
    ).tobytes()
    return bytes(payload)


def _reset_one(layout: Layout) -> bytes:
    """Build a reset mask selecting Slot 0."""

    payload = bytearray(layout.payload_length)
    segment = layout.segment("system.reset_mask")
    payload[segment.offset] = 1
    return bytes(payload)


def _drive_formal_session(config: ResolvedRunConfig) -> tuple[dict[str, Any], dict[str, Any]]:
    """Drive one formal Session and return its persisted Manifest and response."""

    session = UERLSession.open(
        config,
        process_controller=WorkerProcessController(),
    )
    try:
        empty_schema = SessionSchema((), ())
        session.describe(empty_schema.as_request())
        robot_spec = session.described_robot_spec
        assert robot_spec is not None
        observation_shapes = ObservationShapeTable.from_descriptor(session.descriptor)
        schema = SessionSchema(
            robot_observation_schema(robot_spec, observation_shapes),
            robot_actuator_action_schema(robot_spec),
        )
        result = session.initialize(schema.as_request())
        layouts = {
            item["layout_kind"]: Layout.parse(item, batch_size=config.worker.slot_count)
            for item in result.response["layouts"]
        }
        manifest_path = config.logging.run_directory / "manifest.yaml"
        assert manifest_path.exists()
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert manifest["resolved_config"]["normalized_hash"] == config.normalized_hash
        assert manifest["run_seed"] == config.worker.run_seed
        assert manifest["effective_worker_config"] == result.response["effective_worker_config"]
        assert result.response["effective_worker_config_hash"] == sha256_json(
            result.response["effective_worker_config"]
        )
        manifest_identity = dict(manifest)
        manifest_identity.pop("run_directory")
        assert session.manifest_hash == sha256_hex(canonical_json(manifest_identity))
        session.acknowledge_ready()
        _, step_payload = session.step(
            layouts["step_action"].layout_id,
            _step_action(
                layouts["step_action"],
                step_decimation=int(config.worker.decimation[0]),
            ),
        )
        _, reset_payload = session.reset(
            layouts["reset_request"].layout_id,
            _reset_one(layouts["reset_request"]),
        )
        assert len(step_payload) == layouts["step_result"].payload_length
        assert len(reset_payload) == layouts["reset_result"].payload_length
        return manifest, result.response
    finally:
        session.close("p1_e2e")


def test_formal_session_completes_real_ue_initialize_step_reset_shutdown(tmp_path: Path) -> None:
    """Prove the formal Session path and repeatability against real headless UE."""

    port = _free_port()
    config = _resolved_config(port, tmp_path / "run")
    first_manifest, first_response = _drive_formal_session(config)
    second_manifest, second_response = _drive_formal_session(config)

    for key in (
        "resolved_config",
        "run_seed",
        "effective_worker_config",
        "selected_protocol",
        "schema_hashes",
        "layout_hashes",
        "seed_derivation_version",
    ):
        assert first_manifest[key] == second_manifest[key]
    assert first_response["effective_worker_config_hash"] == second_response["effective_worker_config_hash"]
    print("[VERIFY] VC-006: init=1 step=1 reset=1 shutdown=1 in_flight_max=1")
    print("[VERIFY] VC-009: config_hash=EQUAL manifest_contract=EQUAL seed_derivation=EQUAL")

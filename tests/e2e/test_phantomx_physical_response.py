"""Bounded real-UE physical traces through Session, wire decode, and DirectEnv.

No policy or optimizer is involved. The native PhysicsResponse suite owns
analytical mechanics and direct solver-state comparisons. These cases check
that the product feedback path preserves the completed physical trajectory.
"""
from __future__ import annotations

import csv
import math
import struct
from collections.abc import Mapping
from dataclasses import replace
from pathlib import Path
from typing import Any
from unittest.mock import patch

import numpy as np
import pytest
import torch
import yaml

from tests.e2e.support.physical_oracles import joint_consistency
from tests.e2e.support.worker_runner import UE_CMD, UPROJECT
from uerl import (
    ObservationShapeTable,
    PresentationMode,
    ResolvedRunConfig,
    SessionSchema,
    UERLDirectEnv,
    UERLSession,
    UERLSessionAdapter,
    robot_actuator_action_schema,
    robot_observation_schema,
)
from uerl.application.run_config import load_run_config_source
from uerl.core.codec import Layout
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.registry import create_default_registry
from uerl.training import build_launch_overrides, build_run_config

# Literal contract, independent of the production observation/action builder.
JOINTS = tuple(f"{segment}_{leg}" for leg in ("rf", "rm", "rr", "lf", "lm", "lr")
               for segment in ("c1", "thigh", "tibia"))
DEFAULTS = (0.0, 0.15, -0.30) * 6


def _config(directory: Path, slots: int, decimation: int, *, generated_flat: bool = False) -> ResolvedRunConfig:
    base = build_run_config("UERL-PhantomX-Walk-v0")
    overrides = {
        **build_launch_overrides(ue_executable=Path(UE_CMD), project=Path(UPROJECT),
                                 map_name="/Engine/Maps/Entry" if generated_flat else base.session.map_path,
                                 presentation=PresentationMode.NONE),
        "worker.slot_count": str(slots), "worker.run_seed": "0",
        "worker.decimation": f"[{decimation},{decimation}]",
        "task.max_episode_duration_s": "20", "logging.run_directory": str(directory),
    }
    source = load_run_config_source("UERL-PhantomX-Walk-v0", None)
    if generated_flat:
        # The production lattice is sized for many robots. Center this small
        # probe explicitly inside its independently declared 24 m flat patch.
        worker = source.registration.worker_config_factory()
        flat = {
            "num_levels": 1, "cell_size": [24.0, 24.0], "border_width": 1.0,
            "physics_collision": True,
            "tiers": [{"level": 0, "primitive": "plane", "seed": "0", "platform_width": 0.0, "params": {}}],
        }
        # A test-local environment declaration goes through the normal typed
        # resolver. It does not alter the registered production Task.
        source = replace(source, registration=replace(
            source.registration, worker_config_factory=lambda: replace(worker, terrain_config=flat, environment_config={
                **worker.environment_config,
                "environment.columns": float(slots),
                "environment.spacing_x_m": 1.5, "environment.spacing_y_m": 1.5,
                "environment.origin_x_m": -0.75 * (slots - 1), "environment.origin_y_m": 0.0,
            }),
        ))
    return source.resolve(overrides).config


def _wire_state(payload: bytes, layout: Layout, names: tuple[str, ...], slots: int) -> dict[str, np.ndarray[Any, Any]]:
    result = {}
    assert len(payload) == layout.payload_length
    if layout.kind == "step_result":
        valid = layout.segment("system.state_valid")
        faults = layout.segment("system.slot_fault_code")
        assert struct.unpack_from(f"<{slots}B", payload, valid.offset) == (1,) * slots
        assert struct.unpack_from(f"<{slots}H", payload, faults.offset) == (0,) * slots
    for name in names:
        segment = layout.segment(name)
        count = segment.byte_length // 4
        # Independent literal float32 decode, not the Session adapter decoder.
        values = struct.unpack_from(f"<{count}f", payload, segment.offset)
        result[name] = np.asarray(values).reshape(slots, -1)
        assert np.isfinite(result[name]).all(), name
    return result


def _direct_trace(
    directory: Path, decimation: int, *, generated_flat: bool = False,
) -> dict[str, np.ndarray[Any, Any]]:
    config = _config(directory, 1, decimation, generated_flat=generated_flat)
    registry = create_default_registry()
    task = registry.create_task(config.task_id, config.task)
    curriculum = registry.create_curriculum(config.task_id, config.task, num_envs=1, device="cpu", run_seed=0)
    session = UERLSession.open(config, process_controller=WorkerProcessController())
    env: UERLDirectEnv | None = None
    original_step = UERLSession.step
    wire: list[bytes] = []
    rows: list[Mapping[str, Any]] = []

    def capture_step(self: UERLSession, layout_id: int, payload: bytes) -> Any:
        response, state = original_step(self, layout_id, payload)
        wire.append(bytes(state))
        return response, state

    try:
        env = UERLDirectEnv(UERLSessionAdapter(session, device="cpu"), task,
                            resolved_config=config, device="cpu", curriculum_manager=curriculum,
                            step_trace_callback=rows.append)
        assert task.robot_spec is not None
        assert tuple(a.joint for a in task.robot_spec.actuators) == JOINTS
        for actuator, default in zip(task.robot_spec.actuators, DEFAULTS, strict=True):
            assert actuator.target_mode == "position"
            assert actuator.target_unit == "rad"
            assert actuator.stiffness == pytest.approx(25.0)
            assert actuator.damping == pytest.approx(0.5)
            assert actuator.effort_limit == pytest.approx(2.8)
            assert actuator.action_scale == pytest.approx(0.20)
            assert actuator.default_pos == pytest.approx(default)
        assert session.initialization is not None
        definitions = session.initialization.bridge_result.response["layouts"]
        layout = Layout.parse(next(d for d in definitions if d["layout_kind"] == "step_result"), batch_size=1)
        state_names = tuple(str(d["name"]) for d in task.schema.state_requirements)
        action = torch.linspace(-0.1, 0.1, 18).reshape(1, 18)
        expected_targets = [q + 0.2 * float(a) for q, a in zip(DEFAULTS, action[0], strict=True)]
        assert config.worker.physics_dt == pytest.approx(0.005)
        steps = 1600 // decimation
        with patch.object(UERLSession, "step", capture_step):
            for _ in range(steps):
                _, _, terminated, truncated, _ = env.step(action)
                assert not bool((terminated | truncated).any()), "physical trace ended or reset before its horizon"
        assert len(rows) == len(wire) == steps
        traces: dict[str, list[np.ndarray[Any, Any]]] = {name: [] for name in state_names}
        times = []
        with (directory / "physical-joints.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(("time_s", "decimation", "joint", "target_rad", "q_rad", "qd_rad_s"))
            for index, (row, payload) in enumerate(zip(rows, wire, strict=True)):
                state = _wire_state(payload, layout, state_names, 1)
                transition = row["transition"]
                assert transition["state_valid"] and transition["fault_code"] == 0
                assert not transition["terminated"] and not transition["truncated"]
                assert row["clocks"]["step_decimation"] == decimation
                assert row["clocks"]["transition_dt_s"] == pytest.approx(0.005 * decimation)
                timestamp = float(row["clocks"]["transition_episode_elapsed_s"])
                assert timestamp == pytest.approx((index + 1) * 0.005 * decimation, abs=1e-6)
                times.append(timestamp)
                np.testing.assert_allclose(row["action"]["physical_commands"]["robot.actuator.target"],
                                           expected_targets, atol=1e-7, rtol=0)
                for name in state_names:
                    np.testing.assert_array_equal(transition["raw_state"][name], state[name][0], err_msg=name)
                    traces[name].append(state[name][0].copy())
                q = [float(state[f"robot.joint.{joint}.joint_position"][0, 0]) for joint in JOINTS]
                qd = [float(state[f"robot.joint.{joint}.joint_velocity"][0, 0]) for joint in JOINTS]
                policy = transition["observation_groups"]["policy"]
                assert len(policy) == 109
                np.testing.assert_allclose(policy[54:72], np.asarray(q) - DEFAULTS, atol=1e-7, rtol=0)
                np.testing.assert_array_equal(policy[72:90], qd)
                for joint, target, position, velocity in zip(JOINTS, expected_targets, q, qd, strict=True):
                    writer.writerow((timestamp, decimation, joint, target, position, velocity))
        arrays = {name: np.asarray(values) for name, values in traces.items()}
        checks = {}
        # Only D1 gives every solver sample. Never treat sparse D4 rows as a
        # solver-step position/velocity consistency certificate.
        if decimation == 1:
            for joint in JOINTS:
                check = joint_consistency(times[399:], arrays[f"robot.joint.{joint}.joint_position"][399:, 0].tolist(),
                                          arrays[f"robot.joint.{joint}.joint_velocity"][399:, 0].tolist(),
                                          physics_dt=0.005)
                checks[joint] = {
                    "error_rad": check.error, "tolerance_rad": check.tolerance,
                    "maximum_prefix_error_rad": check.maximum_prefix_error,
                    "maximum_prefix_excess_rad": check.maximum_prefix_excess, "passed": check.passed,
                }
            (directory / "physical-summary.yaml").write_text(yaml.safe_dump(checks, sort_keys=False), encoding="utf-8")
            assert all(c["passed"] for c in checks.values()), checks
        return arrays
    finally:
        if env is not None:
            env.close("physical_response_complete")
        else:
            session.close("physical_response_complete")


def test_phantomx_completed_state_wire_observation_and_repeatability(tmp_path: Path) -> None:
    """Run fresh D1, D4 and repeated D4 processes with identical fixed targets."""
    d1 = _direct_trace(tmp_path / "d1", 1)
    d4 = _direct_trace(tmp_path / "d4", 4)
    repeated = _direct_trace(tmp_path / "d4-repeat", 4)
    for joint in JOINTS:
        for suffix, tolerance in (("joint_position", 0.001), ("joint_velocity", 0.01)):
            name = f"robot.joint.{joint}.{suffix}"
            np.testing.assert_allclose(d4[name], d1[name][3::4], atol=tolerance, rtol=0, err_msg=name)
            np.testing.assert_allclose(repeated[name], d4[name], atol=tolerance, rtol=0, err_msg=name)


def _sparse_sequence(directory: Path, reset_selected: bool) -> list[dict[str, np.ndarray[Any, Any]]]:
    config = _config(directory, 2, 1)
    session = UERLSession.open(config, process_controller=WorkerProcessController())
    try:
        session.describe(SessionSchema((), ()).as_request())
        spec = session.described_robot_spec
        assert spec is not None
        shapes = ObservationShapeTable.from_descriptor(session.descriptor)
        schema = SessionSchema(robot_observation_schema(spec, shapes), robot_actuator_action_schema(spec))
        result = session.initialize(schema.as_request())
        layouts = {d["layout_kind"]: Layout.parse(d, batch_size=2) for d in result.response["layouts"]}
        session.acknowledge_ready()
        reset_layout = layouts["reset_request"]
        initial_reset = bytearray(reset_layout.payload_length)
        mask = reset_layout.segment("system.reset_mask")
        assert mask.byte_length == 1
        initial_reset[mask.offset] = 0b11
        reset_segment = reset_layout.segment("robot.reset.values")
        root_pose = (0.0, 0.0, 0.18, 0.0, 0.0, 0.0, 1.0)
        reset_values = []
        for binding in spec.reset:
            if binding.target_type.value == "root_pose":
                reset_values.append(root_pose[binding.component_index])
            else:
                assert binding.target_type.value == "joint_position" and binding.joint in JOINTS
                reset_values.append(DEFAULTS[JOINTS.index(binding.joint)])
        assert reset_segment.byte_length == len(reset_values) * 2 * 4
        struct.pack_into(f"<{len(reset_values) * 2}f", initial_reset, reset_segment.offset,
                         *(reset_values * 2))
        session.reset(reset_layout.layout_id, bytes(initial_reset))
        names = tuple(str(d["name"]) for d in schema.state_requirements)
        states = []
        for step in range(200):
            if step in (50, 100):
                # This public event is a root velocity increment, not a force
                # in newtons. The two Slots receive distinct bounded impulses.
                applied = session.event({
                    "kind": "root_push", "slot_ids": [0, 1],
                    "values": [[0.08, 0.02, 0.0], [-0.05, -0.03, 0.0]],
                })
                assert applied.get("applied") is True and applied.get("kind") == "root_push"
            if step == 100 and reset_selected:
                reset = bytearray(initial_reset)
                reset[mask.offset] = 1
                _, payload = session.reset(reset_layout.layout_id, bytes(reset))
                reset_state = _wire_state(payload, layouts["reset_result"], names, 2)
                for joint, expected in zip(JOINTS, DEFAULTS, strict=True):
                    assert reset_state[f"robot.joint.{joint}.joint_position"][0, 0] == pytest.approx(expected, abs=1e-4)
                    assert reset_state[f"robot.joint.{joint}.joint_velocity"][0, 0] == pytest.approx(0.0, abs=1e-5)
                for name in names:
                    if name.endswith(("body_linear_velocity", "body_angular_velocity", "contact_force")):
                        np.testing.assert_allclose(reset_state[name][0], 0.0, atol=1e-5, rtol=0, err_msg=name)
            action_layout = layouts["step_action"]
            action = bytearray(action_layout.payload_length)
            targets = action_layout.segment("robot.actuator.target")
            # Distinct named patterns expose swapped Slots; no learned action.
            values = [default + sign * 0.015 * math.sin(step * 0.05 + joint * 0.3)
                      for sign in (-1, 1) for joint, default in enumerate(DEFAULTS)]
            struct.pack_into("<36f", action, targets.offset, *values)
            struct.pack_into("<i", action, action_layout.segment("step_decimation").offset, 1)
            _, payload = session.step(action_layout.layout_id, bytes(action))
            states.append(_wire_state(payload, layouts["step_result"], names, 2))
        return states
    finally:
        session.close("physical_sparse_reset_complete")


def test_phantomx_sparse_reset_preserves_unselected_physical_trajectory(tmp_path: Path) -> None:
    """Compare Slot 1 against a fresh same-batch run without an intervening reset."""
    reference = _sparse_sequence(tmp_path / "reference", False)
    selected = _sparse_sequence(tmp_path / "selected-reset", True)
    for before, after in zip(reference, selected, strict=True):
        for name in before:
            tolerance = 0.01 if "velocity" in name else 0.001
            if name.endswith("contact_force"):
                tolerance = 0.1
            np.testing.assert_allclose(after[name][1], before[name][1], atol=tolerance,
                                       rtol=0.005 if name.endswith("contact_force") else 0, err_msg=name)
    assert any(abs(float(row[f"robot.joint.{joint}.joint_velocity"][1, 0])) > 0.01
               for row in reference for joint in JOINTS), "unselected reference must actually move"


def test_phantomx_physical_state_on_authored_and_generated_flat_ground(tmp_path: Path) -> None:
    """Keep scene results separate; check comparable settled support and state."""
    authored = _direct_trace(tmp_path / "authored-flat", 4)
    generated = _direct_trace(tmp_path / "generated-flat", 4, generated_flat=True)
    for label, state in (("authored", authored), ("generated", generated)):
        terrain = state["robot.body.base_link.terrain_height"][100:]
        assert np.max(np.ptp(terrain, axis=1)) < 0.001, f"{label}: fixture is not a flat sampled surface"
        foot_force = sum(state[f"robot.body.tibia_{leg}.contact_force"][100:, 0]
                         for leg in ("rf", "rm", "rr", "lf", "lm", "lr"))
        assert float(np.mean(foot_force)) > 0.1, f"{label}: no measured foot support"
    for joint in JOINTS:
        name = f"robot.joint.{joint}.joint_position"
        np.testing.assert_allclose(np.mean(authored[name][100:], axis=0),
                                   np.mean(generated[name][100:], axis=0), atol=0.02, rtol=0, err_msg=name)
    clearance = "robot.body.base_link.ground_clearance"
    np.testing.assert_allclose(np.mean(authored[clearance][100:], axis=0),
                               np.mean(generated[clearance][100:], axis=0), atol=0.003, rtol=0)


def _assert_native_exchange(
    path: Path, exchanges: list[tuple[int, str, dict[str, np.ndarray[Any, Any]]]], decimation: int,
) -> None:
    """Match independent native geometry, staged values and the same wire sequence."""
    required = {f"robot.joint.{joint}.{quantity}" for joint in JOINTS
                for quantity in ("joint_position", "joint_velocity")}
    required.update(f"robot.body.{joint}.{quantity}[{component}]" for joint in JOINTS
                    for quantity, width in (("body_pose", 7), ("body_linear_velocity", 3), ("body_angular_velocity", 3))
                    for component in range(width))
    with path.open(newline="", encoding="utf-8") as stream:
        columns = ("sequence", "frame", "time", "dt", "phase", "field", "native", "staged")
        rows = list(csv.DictReader(stream, fieldnames=columns))
    assert len(rows) == len(exchanges) * len(required), "missing or extra native physical rows"
    previous_frame: int | None = None
    previous_time: float | None = None
    for index, (sequence, phase, state) in enumerate(exchanges):
        batch = rows[index * len(required):(index + 1) * len(required)]
        assert {row["field"] for row in batch} == required, "missing or duplicate physical fields"
        assert {int(row["sequence"]) for row in batch} == {sequence}, "wrong physical transaction"
        assert phase in ("step", "reset") and {row["phase"] for row in batch} == {phase}
        frames = {int(row["frame"]) for row in batch}
        times = {float(row["time"]) for row in batch}
        assert len(frames) == len(times) == 1, "mixed solver snapshots"
        frame, timestamp = frames.pop(), times.pop()
        assert math.isfinite(timestamp)
        if previous_frame is not None and previous_time is not None:
            expected_steps = decimation if phase == "step" else 0
            assert frame - previous_frame == expected_steps, "wrong completed solver-frame count"
            assert timestamp - previous_time == pytest.approx(0.005 * expected_steps, abs=1e-6)
        previous_frame, previous_time = frame, timestamp
        for row in batch:
            assert float(row["dt"]) == pytest.approx(0.005, abs=1e-7)
            native, staged = float(row["native"]), float(row["staged"])
            assert math.isfinite(native) and math.isfinite(staged)
            field, component = row["field"], 0
            if field.endswith("]"):
                field, suffix = field.rsplit("[", 1)
                component = int(suffix[:-1])
            received = float(state[field][0, component])
            # 17 decimal digits preserve the exact staged float32 promoted to double.
            assert staged == received, f"staging/wire mismatch: {row['field']}"
            assert native == pytest.approx(received, abs=1e-5, rel=1e-6), row["field"]


@pytest.mark.parametrize("decimation", [1, 4])
def test_phantomx_native_completed_state_matches_same_session_exchange(tmp_path: Path, decimation: int) -> None:
    """A rotated robot and reversed schema expose frame, permutation and stale-state errors."""
    directory = tmp_path / f"native-wire-d{decimation}"
    directory.mkdir(parents=True)
    capture = directory / "native-physical-feedback.csv"
    config = _config(directory, 1, decimation, generated_flat=True)
    config = replace(config, session=replace(config.session, worker_args=(
        *config.session.worker_args, f"-uerltestphysicalfeedback={capture}",
    )))
    session = UERLSession.open(config, process_controller=WorkerProcessController())
    exchanges = []
    try:
        session.describe(SessionSchema((), ()).as_request())
        spec = session.described_robot_spec
        assert spec is not None
        fields = robot_observation_schema(spec, ObservationShapeTable.from_descriptor(session.descriptor))
        selected = {str(field["name"]): field for field in fields}
        for field in session.descriptor["available_state_schema"]:
            name = str(field["name"])
            if any(name == f"robot.body.{joint}.{quantity}" for joint in JOINTS
                   for quantity in ("body_pose", "body_linear_velocity", "body_angular_velocity")):
                selected[name] = field
        schema = SessionSchema(tuple(reversed(tuple(selected.values()))), robot_actuator_action_schema(spec))
        result = session.initialize(schema.as_request())
        layouts = {d["layout_kind"]: Layout.parse(d, batch_size=1) for d in result.response["layouts"]}
        session.acknowledge_ready()
        reset_layout = layouts["reset_request"]
        reset = bytearray(reset_layout.payload_length)
        reset[reset_layout.segment("system.reset_mask").offset] = 1
        # Nonzero root yaw does not change the joint-local angle contract.
        pose = (0.0, 0.0, 0.18, 0.0, 0.0, math.sin(0.3), math.cos(0.3))
        values = []
        for binding in spec.reset:
            if binding.target_type.value == "root_pose":
                values.append(pose[binding.component_index])
            else:
                assert binding.target_type.value == "joint_position" and binding.joint in JOINTS
                values.append(DEFAULTS[JOINTS.index(binding.joint)])
        struct.pack_into(f"<{len(values)}f", reset, reset_layout.segment("robot.reset.values").offset, *values)
        names = tuple(str(field["name"]) for field in schema.state_requirements)
        header, state = session.reset(reset_layout.layout_id, bytes(reset))
        exchanges.append((header.sequence, "reset", _wire_state(state, layouts["reset_result"], names, 1)))
        action_layout = layouts["step_action"]
        for step in range(64):
            if step == 32:
                header, state = session.reset(reset_layout.layout_id, bytes(reset))
                exchanges.append((header.sequence, "reset", _wire_state(state, layouts["reset_result"], names, 1)))
            payload = bytearray(action_layout.payload_length)
            targets = [DEFAULTS[JOINTS.index(a.joint)] + 0.01 * math.sin(step * 0.13 + i * 0.4)
                       for i, a in enumerate(spec.actuators)]
            assert len(targets) == 18
            struct.pack_into("<18f", payload, action_layout.segment("robot.actuator.target").offset, *targets)
            struct.pack_into("<i", payload, action_layout.segment("step_decimation").offset, decimation)
            header, state = session.step(action_layout.layout_id, bytes(payload))
            exchanges.append((header.sequence, "step", _wire_state(state, layouts["step_result"], names, 1)))
    finally:
        session.close("native_physical_feedback_complete")
    _assert_native_exchange(capture, exchanges, decimation)

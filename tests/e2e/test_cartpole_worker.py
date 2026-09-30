"""Verify the current SocketBridge-driven UE Worker through black-box E2E tests."""
from __future__ import annotations

from typing import cast

import numpy as np

from tests.e2e.support.phase1 import run_negative_gate, run_repeat, run_stability
from tests.e2e.support.socket_negative import (
    run_active_step_timeout,
    run_envelope_error_matrix,
    run_initialize_failure,
    run_invalid_attached_config,
    run_post_ready_disconnect,
    run_pre_ready_shutdown,
    run_reset_failure,
    run_timeout_and_half_close,
)
from tests.e2e.support.worker_runner import run_pie_attach, run_session


def test_initialize_releases_one_physics_frame_without_starting_an_episode() -> None:
    """Complete Initialize only after one UE-owned physics frame and no Task Step."""
    stats = run_session(num_steps=0)

    assert stats["initialize_stabilization_frames"] == [1]
    assert stats["initialize_flow_ordered"] is True
    assert np.all(stats["initial_episode_indices"] == 0)
    assert stats["final_step_count"] == 0
    print("[VERIFY] Initialize stabilization owns one physics frame and no Task episode")


def test_ac_ue001_normal_worker_step_reset_chain() -> None:
    """Prove real physics, VecEnv shapes, and sparse reset behavior over 200 steps."""
    stats = run_session(num_steps=200)

    assert stats["steps"] == 200
    assert cast(int, stats["resets"]) > 0
    assert cast(float, stats["max_cart_pos"]) > 0.01
    print("[VERIFY] AC-UE-001: real Cart-Pole Step/Reset chain completes")


def test_ac_ue002_invalid_physics_gate_is_rejected() -> None:
    """Reject invalid Worker physics configuration with a stable Initialize error."""
    assert run_negative_gate() is True
    print("[VERIFY] AC-UE-002: invalid physics gate is rejected")


def test_ac_ue003_same_input_is_repeatable() -> None:
    """Verify three same-machine runs stay within the documented tolerance."""
    assert run_repeat(runs=3, steps=120) is True
    print("[VERIFY] AC-UE-003: repeated runs are deterministic within tolerance")


def test_ac_ue004_long_running_worker_remains_stable() -> None:
    """Verify a bounded long run stays alive and observes physical motion."""
    assert run_stability(steps=600) is True
    print("[VERIFY] AC-UE-004: bounded long-run stability holds")


def test_ac_u4_005_destructive_envelope_errors_are_stable() -> None:
    """Verify malformed envelope, config, and state requests return stable errors."""

    run_envelope_error_matrix()
    print("[VERIFY] AC-U4-005: state, sequence, session, config, key, and layout errors are stable")


def test_ac_u4_006_timeout_and_half_close_fail_fast() -> None:
    """Verify handshake, disconnect, and active-step failures stop within deadlines."""

    assert max(run_timeout_and_half_close().values()) <= 2.5
    assert run_pre_ready_shutdown() <= 2.5
    assert run_post_ready_disconnect() <= 2.5
    assert run_active_step_timeout() <= 6.0
    print("[VERIFY] AC-U4-006: timeout, half-close, disconnect, and active cancellation fail fast")


def test_ac_u5_007_none_and_viewport_share_one_physics_path() -> None:
    """Verify rendering changes presentation only, not the fixed input trajectory."""
    headless = run_session(num_steps=60, presentation="none")
    viewport = run_session(num_steps=60, presentation="viewport")

    assert headless["trajectory"] == viewport["trajectory"]
    assert headless["contract"] == viewport["contract"]
    assert headless["map"] == viewport["map"]
    assert headless["final_step_count"] == viewport["final_step_count"] == 60
    assert headless["fixed_step_log_count"] == viewport["fixed_step_log_count"] == 3
    assert headless["rhi"] == "Null"
    assert viewport["rhi"] == "D3D"
    assert viewport["viewport_window"] is True
    assert headless["return_code"] == viewport["return_code"] == 0
    print("[VERIFY] AC-U5-007: NONE and VIEWPORT produce the same fixed-step trajectory")


def test_ac_u5_008_attached_shutdown_preserves_host_process() -> None:
    """Verify an attached Session cleans up Worker resources without owning UE."""
    stats = run_session(num_steps=20, presentation="viewport", attached=True)

    assert stats["host_survived_shutdown"] is True
    assert stats["port_released"] is True
    print("[VERIFY] AC-U5-008: attached Shutdown leaves the UE host alive")


def test_ac_u5_009_failure_cleanup_respects_process_ownership() -> None:
    """Verify Reset WorkerFatal closes resources without killing an attached host."""
    assert run_initialize_failure() is True
    assert run_reset_failure(attached=False) is True
    assert run_reset_failure(attached=True) is True
    assert run_invalid_attached_config() is True
    print("[VERIFY] AC-U5-009: WorkerFatal and invalid attach close only owned resources")


def test_ac_u5_010_editor_pie_attach_can_restart_session() -> None:
    """Verify public StartAttached works in a real PIE GameInstance and can be reused."""
    assert run_pie_attach() is True
    print("[VERIFY] AC-U5-010: Editor/PIE attach public surface is reusable")

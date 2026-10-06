"""Launch a real UE Worker through the product runtime modes end to end."""
from __future__ import annotations

import math
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from pathlib import Path
from typing import Any, TextIO, cast

import torch

from tests.e2e.support.host import resolve_test_host, worker_log_path
from uerl import PresentationMode, UERLDirectEnv, UERLSession, UERLSessionAdapter
from uerl.runtime.session import WorkerProcessController
from uerl.tasks.cartpole import CARTPOLE_TASK_ID, create_cartpole_task
from uerl.training import WORKER_LOCKSTEP_PHYSICS_ARGS, build_launch_overrides, build_run_config

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
HOST_PROFILE = resolve_test_host()
UE_CMD = str(HOST_PROFILE.ue_executable)
UPROJECT = str(HOST_PROFILE.project)
TRAIN_MAP = "/Engine/Maps/Entry"

N = 64
NUM_STEPS = 200
SEED = 0


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def _launch(
    port: int,
    log_file: TextIO,
    extra_args: Sequence[str] | None = None,
    *,
    presentation: str = "none",
    attached: bool = False,
) -> subprocess.Popen[str]:
    if presentation not in {"none", "viewport"}:
        raise ValueError(f"unsupported presentation mode: {presentation}")
    presentation_args = ["-nullrhi"] if presentation == "none" else [
        "-dx11", "-windowed", "-ResX=640", "-ResY=360",
    ]
    return subprocess.Popen(
        [
            UE_CMD, UPROJECT, TRAIN_MAP, "-game", f"-uerlport={port}",
            f"-uerlpresentation={presentation}",
            *presentation_args,
            *(["-uerlattach"] if attached else []),
            "-unattended", "-nopause", "-nosplash", "-nosound", "-stdout", "-FullStdOutLogOutput",
            "-ini:Engine:[/Script/Engine.PhysicsSettings]:bTickPhysicsAsync=False",
            "-ini:Engine:[/Script/Engine.PhysicsSettings]:bSubstepping=False",
            "-ini:Engine:[/Script/Engine.PhysicsSettings]:bSubsteppingAsync=False",
            *(extra_args or []),
        ],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        cwd=REPO_ROOT,
        text=True,
    )


def _launch_resolved(
    worker_executable: Path,
    worker_args: Sequence[str],
    log_file: TextIO,
) -> subprocess.Popen[str]:
    """Launch an externally hosted Worker with resolved Session arguments."""

    return subprocess.Popen(
        [str(worker_executable), *worker_args, "-uerlattach"],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        cwd=REPO_ROOT,
        text=True,
    )


def _stop_process(proc: subprocess.Popen[str]) -> int:
    try:
        return proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            return proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            return proc.wait(timeout=10)


def run_session(
    num_steps: int = NUM_STEPS,
    *,
    presentation: str = "none",
    attached: bool = False,
) -> dict[str, object]:
    """Run the real Worker through the product Session and DirectEnv seams."""

    assert os.path.exists(UE_CMD), f"UnrealEditor-Cmd not found: {UE_CMD}"
    assert os.path.exists(UPROJECT), f"uproject not found: {UPROJECT}"

    port = _free_port()
    ownership = "attached" if attached else "process"
    log_path = worker_log_path(f"_ue_worker_{presentation}_{ownership}")
    presentation_mode = PresentationMode(presentation)
    stats: dict[str, object] = {
        "resets": 0,
        "steps": 0,
        "timeouts": 0,
        "max_cart_pos": 0.0,
        "max_cart_vel": 0.0,
        "presentation": presentation,
        "host_survived_shutdown": False,
    }
    trajectory: list[bytes] = []
    with tempfile.TemporaryDirectory(prefix="uerl-worker-run-") as run_directory:
        overrides = build_launch_overrides(
            ue_executable=Path(UE_CMD),
            project=Path(UPROJECT),
            map_name=TRAIN_MAP,
            port=port,
            presentation=presentation_mode,
        )
        overrides.update({
            "worker.slot_count": str(N),
            "worker.run_seed": str(SEED),
            "logging.run_directory": run_directory,
        })
        overrides["session.mode"] = "attach" if attached else "launch"
        config = build_run_config(CARTPOLE_TASK_ID, overrides=overrides)
        proc: subprocess.Popen[str] | None = None
        raw_session: UERLSession | None = None
        env: UERLDirectEnv | None = None
        return_code = 0
        reset_count = 0
        timeout_count = 0
        max_cart_pos = 0.0
        max_cart_vel = 0.0
        step_count = 0
        controller = WorkerProcessController()
        with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
            if not attached:
                def start_worker(args: Sequence[str], **kwargs: Any) -> subprocess.Popen[str]:
                    return subprocess.Popen(
                        args,
                        stdout=log_file,
                        stderr=subprocess.STDOUT,
                        cwd=REPO_ROOT,
                        text=True,
                        **kwargs,
                    )

                controller = WorkerProcessController(popen_factory=start_worker)
            try:
                if attached:
                    assert config.session.worker_executable is not None
                    proc = _launch_resolved(
                        config.session.worker_executable,
                        config.session.worker_args,
                        log_file,
                    )
                raw_session = UERLSession.open(config, process_controller=controller)
                if not attached:
                    assert controller.handle is not None
                    assert controller.handle.process is not None
                    proc = controller.handle.process
                print(f"[FLOW] launching UE worker port={port}, product Session/DirectEnv")
                task = create_cartpole_task(config.task)
                adapter = UERLSessionAdapter(raw_session)
                env = UERLDirectEnv(
                    adapter,
                    task,
                    resolved_config=config,
                )
                initial_state = adapter.initial_state
                stats["initial_episode_indices"] = (
                    initial_state.episode_index.cpu().numpy()
                )
                initialization = raw_session.initialization
                assert initialization is not None
                init_response = initialization.bridge_result.response
                stats["contract"] = {
                    "selected_protocol": init_response["selected_protocol"],
                    "effective_worker_config": init_response["effective_worker_config"],
                    "effective_worker_config_hash": init_response["effective_worker_config_hash"],
                    "selected_schemas": init_response["selected_schemas"],
                    "layouts": init_response["layouts"],
                    "seed_derivation_version": init_response["seed_derivation_version"],
                }
                stats["map"] = TRAIN_MAP
                observations = env.get_observations()
                assert tuple(observations["policy"].shape) == (N, 4)
                trajectory.append(observations["policy"].contiguous().numpy().tobytes())
                generator = torch.Generator().manual_seed(123)
                for _ in range(num_steps):
                    actions = torch.rand(N, 1, generator=generator) * 2.0 - 1.0
                    observations, _reward, terminated, truncated, _info = env.step(actions)
                    state = observations["policy"]
                    trajectory.append(state.contiguous().numpy().tobytes())
                    assert tuple(state.shape) == (N, 4)
                    assert state.dtype == torch.float32
                    done = terminated | truncated
                    if bool(done.any()):
                        reset_count += int(done.sum().item())
                        post_reset = state[done]
                        assert post_reset[:, 2].abs().max().item() < 0.05
                        assert post_reset[:, 0].abs().max().item() <= 0.25 * math.pi + 1e-3
                    timeout_count += int(truncated.sum().item())
                    max_cart_pos = max(max_cart_pos, float(state[:, 2].abs().max().item()))
                    max_cart_vel = max(max_cart_vel, float(state[:, 3].abs().max().item()))
                    step_count += 1
                stats["steps"] = step_count
                stats["resets"] = reset_count
                stats["timeouts"] = timeout_count
                stats["max_cart_pos"] = max_cart_pos
                stats["max_cart_vel"] = max_cart_vel
                env.close("test_complete")
                env = None
                shutdown_response = cast(dict[str, object] | None, getattr(raw_session, "last_shutdown_response", None))
                assert shutdown_response is not None
                stats["final_step_count"] = int(cast(int, shutdown_response["final_step_count"]))
                assert stats["final_step_count"] == num_steps
                if attached:
                    assert proc is not None
                    time.sleep(0.5)
                    stats["host_survived_shutdown"] = proc.poll() is None
                    try:
                        stale = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                    except OSError:
                        stats["port_released"] = True
                    else:
                        stale.close()
                        stats["port_released"] = False
            finally:
                if env is not None:
                    env.close("test_complete")
                elif raw_session is not None:
                    raw_session.close("test_complete")
                if proc is not None:
                    if attached and proc.poll() is None:
                        proc.terminate()
                    return_code = _stop_process(proc)

    if not attached and return_code != 0:
        raise RuntimeError(f"UE Worker exited abnormally with code {return_code}; see {log_path}")
    stats["trajectory"] = tuple(trajectory)
    stats["return_code"] = return_code
    with open(log_path, encoding="utf-8", errors="replace") as log_file:
        log_text = log_file.read()
    stats["fixed_step_log_count"] = len(re.findall(r"\[VERIFY\] fixed step=", log_text))
    stats["initialize_stabilization_frames"] = [
        int(frame) for frame in re.findall(r"\[VERIFY\] solver baseline frame=(\d+)", log_text)
    ]
    initialize_flow = [
        log_text.find("-> StabilizingInitialize"),
        log_text.find("solver baseline frame="),
        log_text.find("Worker physics stabilized -> WaitingReady"),
    ]
    stats["initialize_flow_ordered"] = (
        all(index >= 0 for index in initialize_flow) and initialize_flow == sorted(initialize_flow)
    )
    if presentation == "viewport":
        assert 'verbatimrhiname="D3D' in log_text
        assert "SlateRHIRenderer" in log_text
        assert "Game Engine Initialized" in log_text
        stats["rhi"] = "D3D"
        stats["viewport_window"] = True
    else:
        assert 'verbatimrhiname="Null"' in log_text
        stats["rhi"] = "Null"
        stats["viewport_window"] = False
    if attached:
        assert "starting explicit attached Worker session" in log_text
        assert "attached Worker session closed without terminating the host" in log_text
        shutdown_order = [
            log_text.rfind("request-driven Frame Gate closed"),
            log_text.rfind("Worker training resources destroyed"),
            log_text.rfind("Worker session deinitialized"),
        ]
        assert all(index >= 0 for index in shutdown_order)
        assert shutdown_order == sorted(shutdown_order)
    return stats


def run_pie_attach() -> bool:
    """Drive StartAttached inside a real Editor PIE GameInstance, then prove reattach.

    Returns:
        ``True`` after two attached sessions, ordered cleanup, and automation
        success markers are observed.
    """
    port = _free_port()
    log_path = worker_log_path("_ue_worker_pie_attach")
    with tempfile.TemporaryDirectory(prefix="uerl-pie-attach-") as run_directory:
        overrides = build_launch_overrides(
            ue_executable=Path(UE_CMD),
            project=Path(UPROJECT),
            map_name=TRAIN_MAP,
            port=port,
            presentation=PresentationMode.NONE,
        )
        overrides.update({
            "session.mode": "attach",
            "worker.slot_count": "2",
            "worker.run_seed": str(SEED),
            "logging.run_directory": run_directory,
        })
        config = build_run_config(CARTPOLE_TASK_ID, overrides=overrides)
        with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
            proc = subprocess.Popen(
                [
                    UE_CMD,
                    UPROJECT,
                    TRAIN_MAP,
                    f"-uerltestpieattachport={port}",
                    "-ExecCmds=Automation RunTests UERL.Integration.Worker.PIEAttach;Quit",
                    *WORKER_LOCKSTEP_PHYSICS_ARGS,
                    "-nullrhi",
                    "-unattended",
                    "-nopause",
                    "-nosplash",
                    "-nosound",
                    "-stdout",
                    "-FullStdOutLogOutput",
                ],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                cwd=REPO_ROOT,
                text=True,
            )
            raw_session: UERLSession | None = None
            env: UERLDirectEnv | None = None
            try:
                raw_session = UERLSession.open(config, process_controller=WorkerProcessController())
                env = UERLDirectEnv(
                    UERLSessionAdapter(raw_session),
                    create_cartpole_task(config.task),
                    resolved_config=config,
                )
                _ = env.step(torch.zeros(2, 1))
                env.close("pie_attach_complete")
                env = None
                shutdown_response = cast(dict[str, object] | None, getattr(raw_session, "last_shutdown_response", None))
                assert shutdown_response is not None
                assert int(cast(int, shutdown_response["final_step_count"])) == 1
                return_code = proc.wait(timeout=180.0)
                if return_code != 0:
                    raise RuntimeError(f"PIE attach automation exited with {return_code}; see {log_path}")
            finally:
                if env is not None:
                    env.close("pie_attach_cleanup")
                elif raw_session is not None:
                    raw_session.close("pie_attach_cleanup")
                if proc.poll() is None:
                    proc.terminate()
                _stop_process(proc)

    with open(log_path, encoding="utf-8", errors="replace") as log_file:
        log_text = log_file.read()
    assert 'verbatimrhiname="Null"' in log_text
    assert "AC-U5-PIE-001: PIE StartAttached listening" in log_text
    assert "AC-U5-PIE-002: PIE host supports cleanup and reattach" in log_text
    session_starts = [match.start() for match in re.finditer(
        "starting explicit attached Worker session", log_text
    )]
    assert len(session_starts) >= 2
    second_session_end = log_text.find("Worker session deinitialized", session_starts[1])
    assert second_session_end >= 0
    session_end_marker = "Worker session deinitialized"
    second_session_log = log_text[
        session_starts[1]:second_session_end + len(session_end_marker)
    ]
    shutdown_order = [
        second_session_log.find("request-driven Frame Gate closed"),
        second_session_log.find("Worker training resources destroyed"),
        second_session_log.find("Worker session deinitialized"),
    ]
    assert all(index >= 0 for index in shutdown_order)
    assert shutdown_order == sorted(shutdown_order)
    assert "Test Completed. Result={Success}" in log_text
    print("[VERIFY] AC-U5-PIE: real Editor/PIE attach, cleanup, and reattach complete")
    return True


def main() -> int:
    """Run the default Worker regression and return a process-style status code."""

    stats = run_session()
    steps = cast(int, stats["steps"])
    resets = cast(int, stats["resets"])
    max_cart_pos = cast(float, stats["max_cart_pos"])
    ok = steps == NUM_STEPS and resets > 0 and max_cart_pos > 0.01
    print(f"[VERIFY] U4 product Runtime steps={steps} resets={resets} max|cart_pos|={max_cart_pos:.4f}")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

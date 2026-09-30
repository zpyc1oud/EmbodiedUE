"""Run destructive real-process checks for the U4 SocketBridge state machine."""
from __future__ import annotations

import os
import socket
import subprocess
import time
from collections.abc import Callable
from copy import deepcopy
from uuid import uuid4

from tests.e2e.support.worker_runner import SEED, N, _free_port, _launch, _stop_process
from tests.protocol.support import protocol
from tests.protocol.support.socket_client import SocketBridgeClient, SocketBridgeError


def _run_error_case(name: str, action: Callable[[SocketBridgeClient], dict[str, object]]) -> str:
    port = _free_port()
    log_path = os.path.join(os.path.dirname(__file__), f"_ue_worker_{name}.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(port, log_file)
        client = SocketBridgeClient("127.0.0.1", port, N, SEED)
        try:
            client.connect(timeout_s=120.0)
            error = action(client)
            code = str(error["code"])
            print(f"[VERIFY] destructive case {name}: {code}")
            return code
        finally:
            if client.sock is not None:
                client.sock.close()
                client.sock = None
            _stop_process(proc)


def run_envelope_error_matrix() -> dict[str, str]:
    """Run malformed envelope cases and compare each stable protocol error code."""

    def unknown_field(client: SocketBridgeClient) -> dict[str, object]:
        """Send an Initialize DTO with an unknown critical field."""

        request = {
            "task_id": "uerl.task.cartpole.v1",
            "task_version": "1.0.0",
            "worker_config": client.worker_config,
            "worker_config_hash": protocol.sha256(client.worker_config),
            "state_requirements": client.state_schema,
            "action_schema": client.action_schema,
            "unknown_critical": True,
        }
        return client.request_error(protocol.INITIALIZE, request)

    def wrong_layout(client: SocketBridgeClient) -> dict[str, object]:
        """Send a Step header with an altered negotiated layout identity."""

        client.initialize()
        layout = client.layouts["step_action"]
        return client.request_header_error(
            protocol.STEP,
            payload_length=protocol.MAX_DATA_PAYLOAD_BYTES,
            layout_id=layout.layout_id ^ 1,
        )

    def step_before_ready(client: SocketBridgeClient) -> dict[str, object]:
        """Send Step after Initialize but before Ready acknowledgement."""

        client.initialize(acknowledge_ready=False)
        layout = client.layouts["step_action"]
        return client.request_header_error(
            protocol.STEP, payload_length=layout.payload_length, layout_id=layout.layout_id
        )

    def legacy_worker_config(client: SocketBridgeClient) -> dict[str, object]:
        """Reject removed v1 Worker DTO fields under protocol v2."""

        worker_config = deepcopy(client.worker_config)
        worker_config["environment"]["reset_distributions"] = {}
        worker_config["safety"] = {"finite_fallback": True}
        request = {
            "task_id": "uerl.task.cartpole.v1",
            "task_version": "1.0.0",
            "worker_config": worker_config,
            "worker_config_hash": protocol.sha256(worker_config),
            "state_requirements": client.state_schema,
            "action_schema": client.action_schema,
            "phase": "describe",
        }
        return client.request_error(protocol.INITIALIZE, request)

    cases: dict[str, tuple[Callable[[SocketBridgeClient], dict[str, object]], str]] = {
        "step_before_ready": (
            step_before_ready,
            "STATE_VIOLATION",
        ),
        "duplicate_sequence": (
            lambda client: client.request_error(protocol.INITIALIZE, {}, sequence=1),
            "SEQUENCE_MISMATCH",
        ),
        "skipped_sequence": (
            lambda client: client.request_error(protocol.INITIALIZE, {}, sequence=3),
            "SEQUENCE_MISMATCH",
        ),
        "session_mismatch": (
            lambda client: client.request_error(protocol.INITIALIZE, {}, session=uuid4()),
            "SESSION_MISMATCH",
        ),
        "unknown_field": (unknown_field, "PAYLOAD_INVALID"),
        # The envelope is valid JSON; the removed fields are rejected while
        # parsing the worker configuration itself.
        "legacy_worker_config": (legacy_worker_config, "CONFIG_REJECTED"),
        "wrong_layout": (wrong_layout, "PAYLOAD_INVALID"),
    }
    results = {name: _run_error_case(name, action) for name, (action, _expected) in cases.items()}
    for name, (_action, expected) in cases.items():
        if results[name] != expected:
            raise AssertionError(f"{name}: expected {expected}, got {results[name]}")
    return results


def _connect_raw(port: int, timeout_s: float = 120.0) -> socket.socket:
    deadline = time.monotonic() + timeout_s
    last_error: OSError | None = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if last_error is not None:
                raise last_error
            raise TimeoutError(f"SocketBridge did not accept a connection on port {port}")
        try:
            connection = socket.create_connection(("127.0.0.1", port), timeout=min(0.1, remaining))
            connection.settimeout(3.0)
            return connection
        except OSError as exc:
            last_error = exc
            time.sleep(min(0.05, remaining))


def run_timeout_and_half_close() -> dict[str, float]:
    """Measure handshake timeout and client half-close failure latency.

    Returns:
        A mapping from destructive case name to elapsed failure seconds.
    """

    durations: dict[str, float] = {}
    for name, send_partial in (("timeout", False), ("half_close", True)):
        port = _free_port()
        log_path = os.path.join(os.path.dirname(__file__), f"_ue_worker_{name}.log")
        with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
            proc = _launch(port, log_file, ["-uerlhandshaketimeoutms=500"])
            connection = _connect_raw(port)
            started = time.monotonic()
            try:
                if send_partial:
                    hello = protocol.FrameHeader(0, 0, protocol.HELLO, protocol.REQUEST, 0, 1, uuid4())
                    connection.sendall(hello.pack()[:17])
                    connection.shutdown(socket.SHUT_WR)
                assert connection.recv(1) == b""
            finally:
                durations[name] = time.monotonic() - started
                connection.close()
                _stop_process(proc)
        if durations[name] > 2.5:
            raise AssertionError(f"{name} did not fail fast: {durations[name]:.3f}s")
        print(f"[VERIFY] destructive case {name}: disconnected in {durations[name]:.3f}s")
    return durations


def run_post_ready_disconnect() -> float:
    """Close a Ready client socket and measure Worker shutdown latency.

    Returns:
        Seconds from the client-side disconnect until the Worker exits.
    """

    port = _free_port()
    log_path = os.path.join(os.path.dirname(__file__), "_ue_worker_post_ready_disconnect.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(port, log_file)
        client = SocketBridgeClient("127.0.0.1", port, 1, SEED)
        try:
            client.connect(timeout_s=120.0)
            if client.hello_response["capabilities"]["viewport"] is not True:
                raise AssertionError("U5 Worker must advertise its verified Viewport capability")
            client.initialize()
            assert client.sock is not None
            started = time.monotonic()
            client.sock.close()
            client.sock = None
            try:
                return_code = proc.wait(timeout=2.5)
            except subprocess.TimeoutExpired as exc:
                raise AssertionError("post-Ready disconnect did not stop the Worker") from exc
            if return_code != 1:
                raise AssertionError(f"post-Ready disconnect exited with {return_code}, expected 1")
            duration = time.monotonic() - started
        finally:
            _stop_process(proc)
    print(f"[VERIFY] destructive case post_ready_disconnect: exited in {duration:.3f}s")
    return duration


def run_pre_ready_shutdown() -> float:
    """Shutdown after Initialize but before Ready and measure Worker exit latency.

    Returns:
        Seconds from the pre-Ready shutdown request until the Worker exits.
    """

    port = _free_port()
    log_path = os.path.join(os.path.dirname(__file__), "_ue_worker_pre_ready_shutdown.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(port, log_file)
        client = SocketBridgeClient("127.0.0.1", port, 1, SEED)
        try:
            client.connect(timeout_s=120.0)
            client.initialize(acknowledge_ready=False)
            started = time.monotonic()
            response = client.shutdown()
            if response.get("final_step_count") != "0":
                raise AssertionError(f"pre-Ready shutdown returned unexpected response: {response}")
            try:
                return_code = proc.wait(timeout=2.5)
            except subprocess.TimeoutExpired as exc:
                raise AssertionError("pre-Ready shutdown did not stop the Worker") from exc
            if return_code != 0:
                raise AssertionError(f"pre-Ready shutdown exited with {return_code}, expected 0")
            duration = time.monotonic() - started
        finally:
            if client.sock is not None:
                client.sock.close()
                client.sock = None
            _stop_process(proc)
    print(f"[VERIFY] destructive case pre_ready_shutdown: exited in {duration:.3f}s")
    return duration


def run_active_step_timeout() -> float:
    """Force an over-deadline active Step and verify physics cancellation.

    Returns:
        Seconds from Step submission until the Worker stops.
    """

    port = _free_port()
    log_path = os.path.join(os.path.dirname(__file__), "_ue_worker_active_step_timeout.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(
            port, log_file,
            ["-uerlrequesttimeoutms=2000", "-uerltestpostphysicsdelayms=3000"],
        )
        client = SocketBridgeClient("127.0.0.1", port, 1, SEED)
        timed_out = False
        try:
            client.connect(timeout_s=120.0)
            client.initialize()
            assert client.sock is not None
            client.sock.settimeout(3.0)
            started = time.monotonic()
            try:
                client.step()
            except (ConnectionError, OSError, SocketBridgeError):
                timed_out = True
            if not timed_out:
                elapsed = time.monotonic() - started
                raise AssertionError(
                    f"long active Step unexpectedly completed in {elapsed:.3f}s within its transaction deadline"
                )
            try:
                return_code = proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired as exc:
                raise AssertionError("timed-out active Step continued running physics") from exc
            if return_code != 1:
                raise AssertionError(f"timed-out active Step exited with {return_code}, expected 1")
            duration = time.monotonic() - started
        finally:
            if client.sock is not None:
                client.sock.close()
                client.sock = None
            _stop_process(proc)
    with open(log_path, encoding="utf-8", errors="replace") as log_file:
        log_text = log_file.read()
        if "SocketBridge TIMEOUT" not in log_text:
            raise AssertionError("active Step stopped without the expected transaction timeout evidence")
        if "Bridge failure stopped Worker with remaining_frames=1" not in log_text:
            raise AssertionError("timeout did not stop the active Step with one physics frame still gated")
    print(f"[VERIFY] destructive case active_step_timeout: stopped in {duration:.3f}s")
    return duration


def run_reset_failure(*, attached: bool) -> bool:
    """Force Reset failure and verify cleanup respects process ownership.

    Args:
        attached: Whether the Worker host is externally owned and must survive.

    Returns:
        ``True`` after log evidence and ownership assertions pass.
    """

    ownership = "attached" if attached else "process"
    port = _free_port()
    log_path = os.path.join(os.path.dirname(__file__), f"_ue_worker_reset_failure_{ownership}.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(
            port,
            log_file,
            ["-uerltestresetfailure"],
            attached=attached,
        )
        client = SocketBridgeClient("127.0.0.1", port, 1, SEED)
        try:
            client.connect(timeout_s=120.0)
            client.initialize()
            try:
                client.reset([0])
            except SocketBridgeError as exc:
                if "WORKER_FATAL" not in str(exc) or "forced Reset failure" not in str(exc):
                    raise
            else:
                raise AssertionError("forced Reset failure unexpectedly succeeded")

            if attached:
                time.sleep(0.5)
                if proc.poll() is not None:
                    raise AssertionError("attached Reset failure terminated the host")
                try:
                    stale = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                except OSError:
                    pass
                else:
                    stale.close()
                    raise AssertionError("attached Reset failure left the Bridge port open")
            else:
                return_code = proc.wait(timeout=3.0)
                if return_code != 1:
                    raise AssertionError(f"Reset failure exited with {return_code}, expected 1")
        finally:
            if client.sock is not None:
                client.sock.close()
                client.sock = None
            if attached and proc.poll() is None:
                proc.terminate()
            _stop_process(proc)

    with open(log_path, encoding="utf-8", errors="replace") as log_file:
        log_text = log_file.read()
    if "request-driven Frame Gate closed" not in log_text:
        raise AssertionError("Reset failure did not close the Frame Gate")
    if "Worker training resources destroyed slots=1 remaining_frames=0" not in log_text:
        raise AssertionError("Reset failure did not destroy the training Slot")
    if attached and "attached Worker session closed without terminating the host" not in log_text:
        raise AssertionError("attached Reset failure did not close only the Session")
    print(f"[VERIFY] destructive case reset_failure_{ownership}: resources closed")
    return True


def run_initialize_failure() -> bool:
    """Fail after Pool creation and verify no initialized resources survive."""

    port = _free_port()
    log_path = os.path.join(os.path.dirname(__file__), "_ue_worker_initialize_failure.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(port, log_file, ["-uerltestinitializefailure"])
        client = SocketBridgeClient("127.0.0.1", port, N, SEED)
        try:
            client.connect(timeout_s=120.0)
            try:
                client.initialize()
            except SocketBridgeError as exc:
                if "WORKER_FATAL" not in str(exc) or "forced Initialize failure" not in str(exc):
                    raise
            else:
                raise AssertionError("forced Initialize failure unexpectedly succeeded")
            return_code = proc.wait(timeout=3.0)
            if return_code != 1:
                raise AssertionError(f"Initialize failure exited with {return_code}, expected 1")
        finally:
            if client.sock is not None:
                client.sock.close()
                client.sock = None
            _stop_process(proc)

    with open(log_path, encoding="utf-8", errors="replace") as log_file:
        log_text = log_file.read()
    if "request-driven Frame Gate closed" not in log_text:
        raise AssertionError("Initialize failure did not close the Frame Gate")
    if f"Worker training resources destroyed slots={N} remaining_frames=0" not in log_text:
        raise AssertionError("Initialize failure did not destroy the partially initialized Pool")
    print("[VERIFY] destructive case initialize_failure: partial Pool destroyed")
    return True


def run_invalid_attached_config() -> bool:
    """Reject an invalid attached command line without killing the host.

    Returns:
        ``True`` when the host remains alive and no Bridge listener opens.
    """

    port = _free_port()
    log_path = os.path.join(os.path.dirname(__file__), "_ue_worker_invalid_attached_config.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(
            port,
            log_file,
            ["-nullrhi"],
            presentation="viewport",
            attached=True,
        )
        try:
            deadline = time.monotonic() + 120.0
            while time.monotonic() < deadline:
                log_file.flush()
                with open(log_path, encoding="utf-8", errors="replace") as reader:
                    if "rejected Worker command line" in reader.read():
                        break
                if proc.poll() is not None:
                    raise AssertionError("invalid attached config terminated the host")
                time.sleep(0.05)
            else:
                raise AssertionError("invalid attached config did not report its rejection")
            if proc.poll() is not None:
                raise AssertionError("invalid attached config terminated the host")
            try:
                stale = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            except OSError:
                pass
            else:
                stale.close()
                raise AssertionError("invalid attached config opened a Bridge listener")
        finally:
            if proc.poll() is None:
                proc.terminate()
            _stop_process(proc)
    print("[VERIFY] destructive case invalid_attached_config: host preserved")
    return True

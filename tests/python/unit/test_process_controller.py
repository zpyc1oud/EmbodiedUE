"""Verify Worker launch / attach ownership semantics."""

import subprocess
from pathlib import Path
from typing import cast

import pytest

from uerl.core.config import LaunchMode, SessionConfig
from uerl.errors import SessionError
from uerl.runtime.session import WorkerOwnership, WorkerProcessController
from uerl.runtime.session.process import PopenFactory


class _FakeProcess:
    """Record process cleanup calls without starting an operating-system child."""

    def __init__(self, *, exits_naturally: bool = False) -> None:
        self.pid = 1234
        self.exits_naturally = exits_naturally
        self.terminated = False
        self.killed = False
        self.waited = False

    def poll(self) -> int | None:
        """Report exit only after the fake process is terminated or killed."""

        return 0 if self.terminated or self.killed else None

    def terminate(self) -> None:
        """Record graceful termination requested by ownership cleanup."""

        self.terminated = True

    def kill(self) -> None:
        """Record forced termination after a graceful wait timeout."""

        self.killed = True

    def wait(self, timeout: float | None = None) -> int:
        """Record a wait and return a successful fake exit code."""

        self.waited = True
        if not self.exits_naturally and not self.terminated and not self.killed:
            raise subprocess.TimeoutExpired("fake-worker", timeout or 0.0)
        return 0


def test_launch_owns_and_reclaims_its_popen_handle() -> None:
    """Terminate the exact process created by launch mode."""

    process = _FakeProcess()
    calls: list[list[str]] = []

    def popen(args: list[str], **_kwargs: object) -> _FakeProcess:
        """Record launch arguments and return the exact fake process handle."""

        calls.append(args)
        return process

    controller = WorkerProcessController(popen_factory=cast(PopenFactory, popen))
    handle = controller.launch(
        SessionConfig(
            mode=LaunchMode.LAUNCH,
            worker_executable=Path("Worker.exe"),
            worker_args=("--port", "1234"),
        ),
        extra_args=("-uerlperformancepath=C:/run/worker_stage_latency.jsonl",),
    )

    assert handle.ownership is WorkerOwnership.PROCESS
    assert calls == [[
        "Worker.exe",
        "--port",
        "1234",
        "-uerlperformancepath=C:/run/worker_stage_latency.jsonl",
    ]]
    controller.close("test")
    assert process.terminated
    assert process.waited
    assert controller.handle is None
    print("[VERIFY] VC-004: launch_owned_exit=true attached_host_alive=true")


def test_close_waits_for_graceful_process_exit_before_forcing_termination() -> None:
    """Allow a process-owned Worker to honor its protocol Shutdown exit code."""

    process = _FakeProcess(exits_naturally=True)
    controller = WorkerProcessController(popen_factory=cast(PopenFactory, lambda _args, **_kwargs: process))
    controller.launch(
        SessionConfig(
            mode=LaunchMode.LAUNCH,
            worker_executable=Path("Worker.exe"),
        )
    )

    controller.close("test")

    assert process.waited
    assert not process.terminated
    assert not process.killed


def test_attach_does_not_own_or_terminate_external_host() -> None:
    """Keep attach close from touching an external Editor or PIE host."""

    calls: list[list[str]] = []
    controller = WorkerProcessController(popen_factory=cast(PopenFactory, lambda args, **_kwargs: calls.append(args)))

    handle = controller.attach(SessionConfig(mode=LaunchMode.ATTACH))

    assert handle.ownership is WorkerOwnership.ATTACHED
    assert handle.process is None
    controller.close("test")
    assert calls == []


def test_launch_requires_an_executable_and_correct_mode() -> None:
    """Reject invalid launch input at the process boundary."""

    controller = WorkerProcessController(popen_factory=cast(PopenFactory, lambda _args, **_kwargs: _FakeProcess()))

    with pytest.raises(SessionError, match="executable"):
        controller.launch(SessionConfig(mode=LaunchMode.LAUNCH))
    with pytest.raises(SessionError, match="LaunchMode"):
        controller.launch(SessionConfig(mode=LaunchMode.ATTACH, worker_executable=Path("Worker.exe")))

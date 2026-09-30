"""Control launch / attach Worker ownership at the OS process boundary."""

from __future__ import annotations

import subprocess
from collections.abc import Sequence
from dataclasses import dataclass
from enum import StrEnum
from typing import Any, Protocol

from ...core.config.models import LaunchMode, SessionConfig
from ...errors import SessionError


class WorkerOwnership(StrEnum):
    """Identify whether Python owns the Worker process."""

    PROCESS = "process"
    ATTACHED = "attached"


@dataclass(frozen=True, slots=True)
class ProcessHandle:
    """Record the ownership and process handle created for one Session.

    ``PROCESS`` handles contain the exact ``Popen`` object created by launch;
    ``ATTACHED`` handles contain no process object because Python must not
    terminate an externally owned Worker.
    """

    ownership: WorkerOwnership
    process: subprocess.Popen[Any] | None


class PopenFactory(Protocol):
    """Create a child process for launch mode."""

    def __call__(self, args: Sequence[str], **kwargs: Any) -> subprocess.Popen[Any]:
        """Start one Worker child process.

        Args:
            args: Executable and arguments passed to the child process.
            kwargs: Process-construction options owned by the controller.

        Returns:
            The exact process handle retained for later ownership cleanup.
        """


class WorkerProcessController:
    """Launch or attach one Worker without scanning or killing process names."""

    def __init__(self, popen_factory: PopenFactory | None = None, close_timeout_s: float = 5.0) -> None:
        """Create an empty ownership controller.

        Args:
            popen_factory: Optional process factory for launch tests or custom
                process creation.
            close_timeout_s: Maximum wait for graceful termination and, after
                kill, the maximum wait for final process exit.
        """

        self._popen_factory = popen_factory or subprocess.Popen
        self._close_timeout_s = close_timeout_s
        self._handle: ProcessHandle | None = None

    @property
    def handle(self) -> ProcessHandle | None:
        """Return the current ownership handle, if a Session is open.

        Returns:
            The exact launch/attach record, or ``None`` after close and before
            the first ownership operation.
        """

        return self._handle

    def launch(self, config: SessionConfig, *, extra_args: Sequence[str] = ()) -> ProcessHandle:
        """Launch a Worker and retain the exact Popen handle for cleanup.

        Args:
            config: A ``LaunchMode.LAUNCH`` SessionConfig with a Worker
                executable and its argument tuple.

        Returns:
            A ``PROCESS`` handle containing the created child process.

        Raises:
            SessionError: If the mode, executable, controller state, or OS
                launch operation is invalid.

        Side effects:
            Start one child process and make this controller non-empty. The
            controller never discovers or terminates unrelated processes.
        """

        if config.mode is not LaunchMode.LAUNCH:
            raise SessionError("launch requires LaunchMode.LAUNCH")
        if config.worker_executable is None:
            raise SessionError("launch requires a Worker executable")
        self._ensure_empty()
        args = [str(config.worker_executable), *config.worker_args, *extra_args]
        try:
            process = self._popen_factory(args, stdin=subprocess.DEVNULL)
        except OSError as exc:
            raise SessionError("Worker process launch failed") from exc
        self._handle = ProcessHandle(WorkerOwnership.PROCESS, process)
        return self._handle

    def attach(self, config: SessionConfig) -> ProcessHandle:
        """Record an external host attachment without taking process ownership.

        Args:
            config: An ``ATTACH`` SessionConfig whose external Worker endpoint
                is already expected to exist.

        Returns:
            An ``ATTACHED`` handle with no owned process object.

        Raises:
            SessionError: If the mode is not ATTACH or this controller already
                owns a launch/attach record.

        Side effects:
            Record attachment state only. No process is started, scanned, or
            stopped.
        """

        if config.mode is not LaunchMode.ATTACH:
            raise SessionError("attach requires LaunchMode.ATTACH")
        self._ensure_empty()
        self._handle = ProcessHandle(WorkerOwnership.ATTACHED, None)
        return self._handle

    def close(self, reason: str = "session_close") -> None:
        """Close the owned process only; an attached host remains running.

        Args:
            reason: Audit text accepted for Session symmetry; process termination
                itself does not depend on this value.

        Side effects:
            Clear the ownership handle. A launched process is terminated and
            killed only after the graceful wait expires; an attached process is
            never touched. Repeated calls are no-ops.
        """

        handle, self._handle = self._handle, None
        if handle is None or handle.ownership is WorkerOwnership.ATTACHED or handle.process is None:
            return
        process = handle.process
        if process.poll() is not None:
            return
        try:
            process.wait(timeout=self._close_timeout_s)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=self._close_timeout_s)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=self._close_timeout_s)

    def _ensure_empty(self) -> None:
        if self._handle is not None:
            raise SessionError("Worker process controller already owns a session")

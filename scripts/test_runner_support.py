"""Bound test subprocesses, render live progress, and retain one YAML report."""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Sequence
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import asdict, dataclass
from pathlib import Path

import yaml

from tests.e2e.support.host import host_environment, resolve_test_host
from uerl.cli.host_flags import add_host_flags
from uerl.errors import ConfigError
from uerl.host import HostProfile

REPO_ROOT = Path(__file__).resolve().parents[1]
_REAL_POPEN = subprocess.Popen

_AUTOMATION_DONE_RE = re.compile(
    r"Test Completed\. Result=\{([^}]+)\} Name=\{([^}]*)\} Path=\{([^}]+)\}"
)
_PYTEST_COLLECTED_RE = re.compile(r"collected\s+(\d+)\s+items?(?:\s*/\s*\d+\s+deselected\s*/\s*(\d+)\s+selected)?")
_PYTEST_VERBOSE_CASE_RE = re.compile(r"(?m)^(tests/\S+::\S+)")
_PYTEST_VERBOSE_OUTCOME_RE = re.compile(
    r"(?m)(?:^|\s)(PASSED|FAILED|SKIPPED|ERROR|XFAIL|XPASS)(?:\s+\[\s*(\d+)%\])?\s*$"
)
_PYTEST_QUIET_DOTS_RE = re.compile(r"(?m)^([.FEsxX]+)(?:\s+\[\s*(\d+)%\])?\s*$")


def positive_seconds(value: str) -> float:
    """Reject zero, negative, and non-finite stage deadlines."""
    import math

    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("timeout must be a finite positive number of seconds")
    return seconds


def positive_int(value: str) -> int:
    """Reject zero, negative, or non-integer concurrency counts."""

    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("count must be a positive integer")
    return parsed


def add_runner_flags(parser: argparse.ArgumentParser) -> None:
    add_host_flags(parser)
    parser.add_argument(
        "--timeout",
        type=positive_seconds,
        default=1800.0,
        help="Timeout per stage in seconds (1800).",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=REPO_ROOT / ".scratch/test-runs",
        help="Parent for a new, unique report directory.",
    )
    parser.add_argument(
        "--no-progress",
        action="store_true",
        help="Disable live progress bar updates.",
    )


def selected_host(args: argparse.Namespace) -> HostProfile:
    profile = resolve_test_host(
        profile_path=args.host_profile,
        ue_executable=args.ue_executable,
        project=args.project,
    )
    print(f"Host profile: {profile.profile_path}", flush=True)
    print(f"UE executable: {profile.ue_executable} ({profile.sources['ue_executable']})", flush=True)
    print(f"UE project: {profile.project} ({profile.sources['project']})", flush=True)
    return profile


@dataclass
class StageResult:
    name: str
    status: str = "not_run"
    command: list[str] | None = None
    returncode: int | None = None
    pid: int | None = None
    duration_s: float = 0.0
    log: str | None = None
    detail: str | None = None


@dataclass(frozen=True)
class StageProgressSpec:
    total: int | None = None
    log_path: Path | None = None
    kind: str = "auto"


@dataclass(frozen=True)
class ProgressSnapshot:
    done: int = 0
    total: int | None = None
    percent: int | None = None
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    last_case: str = ""


def _infer_stage_kind(command: Sequence[str], explicit_kind: str) -> str:
    if explicit_kind != "auto":
        return explicit_kind
    if any(arg.startswith("-abslog=") for arg in command):
        return "automation"
    if any("pytest" in os.path.basename(arg) or arg == "pytest" for arg in command):
        return "pytest"
    return "generic"


def _resolve_progress_log(
    spec: StageProgressSpec | None,
    command: Sequence[str],
    stdout_log: Path,
) -> tuple[str, Path, int | None]:
    kind = _infer_stage_kind(command, "auto" if spec is None else spec.kind)
    total = None if spec is None else spec.total
    if spec is not None and spec.log_path is not None:
        return kind, spec.log_path, total
    if kind == "automation":
        abslog = next((arg.removeprefix("-abslog=") for arg in command if arg.startswith("-abslog=")), None)
        if abslog:
            return kind, Path(abslog), total
    return kind, stdout_log, total


def _short_case_name(case: str, max_len: int = 40) -> str:
    if len(case) <= max_len:
        return case
    if "::" in case:
        _, func = case.rsplit("::", 1)
        if len(func) <= max_len:
            return func
    return "..." + case[-(max_len - 3) :]


def parse_progress_from_text(
    kind: str,
    text: str,
    *,
    configured_total: int | None = None,
) -> ProgressSnapshot:
    """Extract completed counts and latest test name from UE Automation or pytest logs."""

    if kind == "automation":
        matches = _AUTOMATION_DONE_RE.findall(text)
        done = len(matches)
        passed = sum(1 for result, _, _ in matches if result == "Success")
        failed = done - passed
        percent = (
            min(100, int((done * 100) / configured_total))
            if configured_total and configured_total > 0
            else None
        )
        last_case = _short_case_name(matches[-1][2]) if matches else ""
        return ProgressSnapshot(
            done=done,
            total=configured_total,
            percent=percent,
            passed=passed,
            failed=failed,
            last_case=last_case,
        )

    if kind == "pytest":
        collected_match = _PYTEST_COLLECTED_RE.search(text)
        total: int | None
        if collected_match:
            selected_str = collected_match.group(2)
            total = int(selected_str) if selected_str is not None else int(collected_match.group(1))
        else:
            total = configured_total
        verbose_outcomes = _PYTEST_VERBOSE_OUTCOME_RE.findall(text)
        case_matches = _PYTEST_VERBOSE_CASE_RE.findall(text)
        last_case = _short_case_name(case_matches[-1]) if case_matches else ""
        if verbose_outcomes:
            passed = sum(1 for tag, _ in verbose_outcomes if tag in {"PASSED", "XPASS"})
            failed = sum(1 for tag, _ in verbose_outcomes if tag in {"FAILED", "ERROR"})
            skipped = sum(1 for tag, _ in verbose_outcomes if tag in {"SKIPPED", "XFAIL"})
            done = len(verbose_outcomes)
            pct_str = next((pct for _, pct in reversed(verbose_outcomes) if pct), "")
            percent = (
                min(100, int((done * 100) / total))
                if total and total > 0
                else (int(pct_str) if pct_str else None)
            )
            return ProgressSnapshot(
                done=done,
                total=total,
                percent=percent,
                passed=passed,
                failed=failed,
                skipped=skipped,
                last_case=last_case,
            )

        quiet_chunks = _PYTEST_QUIET_DOTS_RE.findall(text)
        if quiet_chunks:
            passed = 0
            failed = 0
            skipped = 0
            pct_str = ""
            for symbols, pct in quiet_chunks:
                for ch in symbols:
                    if ch in {".", "X"}:
                        passed += 1
                    elif ch in {"F", "E"}:
                        failed += 1
                    elif ch in {"s", "x"}:
                        skipped += 1
                if pct:
                    pct_str = pct
            done = passed + failed + skipped
            percent = (
                min(100, int((done * 100) / total))
                if total and total > 0
                else (int(pct_str) if pct_str else None)
            )
            return ProgressSnapshot(
                done=done,
                total=total,
                percent=percent,
                passed=passed,
                failed=failed,
                skipped=skipped,
                last_case=last_case,
            )
        return ProgressSnapshot(total=total, last_case=last_case)

    return ProgressSnapshot(total=configured_total)


def _read_tail_for_progress(path: Path, max_bytes: int = 262144) -> str:
    try:
        size = path.stat().st_size
        with path.open("rb") as handle:
            if size > max_bytes:
                handle.seek(-max_bytes, os.SEEK_END)
            return handle.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def _read_full_for_progress(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _format_elapsed(seconds: float) -> str:
    total = max(0, int(seconds))
    minutes, secs = divmod(total, 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        return f"{hours:02d}:{minutes:02d}:{secs:02d}"
    return f"{minutes:02d}:{secs:02d}"


def format_progress_line(
    label: str,
    snapshot: ProgressSnapshot,
    elapsed_s: float,
    *,
    width: int = 18,
    ascii_only: bool = False,
) -> str:
    """Format one compact single-line progress bar string."""

    fill_ch = "#" if ascii_only else "\u2588"
    empty_ch = "-" if ascii_only else "\u2591"
    pct = snapshot.percent
    if pct is None and snapshot.total and snapshot.total > 0:
        pct = min(100, int((snapshot.done * 100) / snapshot.total))

    if pct is not None:
        filled = min(width, max(0, int(round(width * pct / 100.0))))
        bar = f"[{fill_ch * filled}{empty_ch * (width - filled)}] {pct:3d}%"
    else:
        pos = int(elapsed_s * 2) % width
        chars = [empty_ch] * width
        chars[pos] = fill_ch
        bar = f"[{''.join(chars)}]"

    if snapshot.total is not None and snapshot.total > 0:
        counts = f"{snapshot.done}/{snapshot.total}"
    elif snapshot.done > 0:
        counts = f"done={snapshot.done}"
    else:
        counts = "running"

    status_bits = f"pass={snapshot.passed}"
    if snapshot.failed:
        status_bits += f" fail={snapshot.failed}"
    if snapshot.skipped:
        status_bits += f" skip={snapshot.skipped}"

    parts = [label, bar, counts]
    if snapshot.done > 0:
        parts.append(status_bits)
    parts.append(_format_elapsed(elapsed_s))
    if snapshot.last_case:
        parts.append(snapshot.last_case)
    return " | ".join(parts)


class _ProgressRenderer:
    """Render live progress inline on a TTY or throttled lines when redirected."""

    def __init__(self, *, enabled: bool) -> None:
        self._enabled = enabled
        self._is_tty = bool(getattr(sys.stdout, "isatty", lambda: False)())
        encoding = (getattr(sys.stdout, "encoding", "") or "").lower()
        self._ascii_only = "utf" not in encoding
        self._last_len = 0
        self._last_non_tty_emit = -10.0
        self._last_key: tuple[int, int | None, int | None, str] | None = None

    def update(self, label: str, snapshot: ProgressSnapshot, elapsed_s: float, *, force: bool = False) -> None:
        if not self._enabled:
            return
        line = format_progress_line(
            label,
            snapshot,
            elapsed_s,
            ascii_only=self._ascii_only,
        )
        if self._is_tty:
            pad = max(0, self._last_len - len(line))
            sys.stdout.write("\r" + line + (" " * pad))
            sys.stdout.flush()
            self._last_len = len(line)
            return

        key = (snapshot.done, snapshot.total, snapshot.percent, snapshot.last_case)
        now = time.monotonic()
        has_progress = snapshot.done > 0 or snapshot.percent is not None
        if (
            force
            or (now - self._last_non_tty_emit >= 5.0)
            or (key != self._last_key and has_progress and (now - self._last_non_tty_emit >= 2.0))
        ):
            print(f"[PROGRESS] {line}", flush=True)
            self._last_non_tty_emit = now
            self._last_key = key

    def finish(self) -> None:
        if not self._enabled:
            return
        if self._is_tty and self._last_len > 0:
            sys.stdout.write("\r" + (" " * self._last_len) + "\r")
            sys.stdout.flush()
            self._last_len = 0


def _stop_windows_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        # The PID comes from this Popen handle, never a process-name search.
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
            timeout=20,
        )
    process.wait(timeout=10)


def stop_owned_process(process: subprocess.Popen[bytes]) -> None:
    """Stop only this invocation's child tree or POSIX process group."""
    if sys.platform == "win32":
        _stop_windows_process(process)
    else:
        # Every stage starts a new session. Its children inherit this group.
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        # A parent may exit before a child that ignores SIGTERM.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=10)


class TestRun:
    """Keep stage results, captured output, and environment selection together."""

    __test__ = False

    def __init__(
        self,
        parent: Path,
        stage_names: list[str],
        timeout: float,
        *,
        show_progress: bool = True,
    ) -> None:
        parent = parent.expanduser().resolve()
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="validation-", dir=parent))
        self.timeout = timeout
        self.show_progress = show_progress
        self.stages = [StageResult(name) for name in stage_names]
        self._stage_progress: dict[int, StageProgressSpec] = {}
        self._lock = threading.Lock()
        self.host: HostProfile | None = None
        self.save()
        print(f"Test report: {self.directory / 'summary.yaml'}", flush=True)

    def set_stage_progress(
        self,
        index: int,
        *,
        total: int | None = None,
        log_path: Path | None = None,
        kind: str = "auto",
    ) -> None:
        """Configure optional total/log overrides for live progress rendering."""

        self._stage_progress[index] = StageProgressSpec(total=total, log_path=log_path, kind=kind)

    def save(self) -> None:
        with self._lock:
            self._save_unlocked()

    def _save_unlocked(self) -> None:
        host = (
            None
            if self.host is None
            else {
                "profile_path": str(self.host.profile_path),
                "ue_executable": str(self.host.ue_executable),
                "project": str(self.host.project),
                "sources": self.host.sources,
            }
        )
        report = {
            "timeout_s": self.timeout,
            "host": host,
            "stages": [asdict(stage) for stage in self.stages],
        }
        temporary = self.directory / "summary.yaml.tmp"
        temporary.write_text(
            yaml.safe_dump(report, sort_keys=False, allow_unicode=True),
            encoding="utf-8",
        )
        temporary.replace(self.directory / "summary.yaml")

    def configure_host(self, args: argparse.Namespace, stage_index: int) -> bool:
        try:
            self.host = selected_host(args)
            missing = [path for path in (self.host.ue_executable, self.host.project) if not path.is_file()]
            if missing:
                raise ConfigError(
                    "UE test prerequisites are missing: " + ", ".join(str(path) for path in missing)
                )
        except (ConfigError, OSError) as exc:
            self.stages[stage_index].status = "blocked"
            self.stages[stage_index].detail = str(exc)
            print(str(exc), flush=True)
            self.save()
            return False
        self.save()
        return True

    def _build_environment(self) -> dict[str, str]:
        environment = (
            os.environ.copy()
            if self.host is None
            else host_environment(self.host, self.directory)
        )
        source_paths = [str(REPO_ROOT / "src"), str(REPO_ROOT)]
        if environment.get("PYTHONPATH"):
            source_paths.append(environment["PYTHONPATH"])
        environment["PYTHONPATH"] = os.pathsep.join(source_paths)
        environment.setdefault("PYTHONUNBUFFERED", "1")
        return environment

    def _read_stage_snapshot(
        self,
        index: int,
        command: Sequence[str],
        stdout_log: Path,
        *,
        full: bool = False,
    ) -> ProgressSnapshot:
        kind, target_log, total = _resolve_progress_log(
            self._stage_progress.get(index),
            command,
            stdout_log,
        )
        text = _read_full_for_progress(target_log) if full or total is not None else _read_tail_for_progress(target_log)
        if kind == "pytest" and not full and total is None and text:
            text = _read_full_for_progress(target_log)
        return parse_progress_from_text(kind, text, configured_total=total)

    def _wait_process_with_progress(
        self,
        process: subprocess.Popen[bytes],
        index: int,
        command: Sequence[str],
        stdout_log: Path,
        start: float,
        renderer: _ProgressRenderer,
    ) -> int:
        if not isinstance(process, _REAL_POPEN):
            return process.wait(timeout=self.timeout)

        deadline = start + self.timeout
        stage = self.stages[index]
        label = f"[{index + 1}/{len(self.stages)}] {stage.name}"
        poll_slice = 0.25 if self.show_progress else min(1.0, self.timeout)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(command, self.timeout)
            try:
                return process.wait(timeout=min(poll_slice, remaining))
            except subprocess.TimeoutExpired:
                if self.show_progress:
                    snap = self._read_stage_snapshot(index, command, stdout_log)
                    renderer.update(label, snap, time.monotonic() - start)

    def run(self, index: int, command: list[str]) -> int:
        stage = self.stages[index]
        stage.command = command
        stage.status = "running"
        log_path = self.directory / f"{index + 1:02d}.stdout.log"
        stage.log = str(log_path)
        environment = self._build_environment()
        self.save()
        print(f"Running {stage.name}; output: {log_path}", flush=True)
        start = time.monotonic()
        process: subprocess.Popen[bytes] | None = None
        renderer = _ProgressRenderer(enabled=self.show_progress)
        status = 2
        try:
            with log_path.open("wb") as output:
                process = subprocess.Popen(
                    command,
                    cwd=REPO_ROOT,
                    env=environment,
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    start_new_session=os.name != "nt",
                )
                stage.pid = process.pid
                self.save()
                stage.returncode = self._wait_process_with_progress(
                    process,
                    index,
                    command,
                    log_path,
                    start,
                    renderer,
                )
                status = stage.returncode
                stage.status = "passed" if status == 0 else "failed"
        except subprocess.TimeoutExpired:
            stage.status = "timed_out"
            stage.detail = f"Stage exceeded {self.timeout:g} seconds."
            status = 124
        except KeyboardInterrupt:
            stage.status = "interrupted"
            stage.detail = "Interrupted by the operator."
            status = 130
        except OSError as exc:
            stage.status = "failed"
            stage.detail = str(exc)
        finally:
            if process is not None and (
                process.poll() is None or stage.status in {"timed_out", "interrupted"}
            ):
                try:
                    stop_owned_process(process)
                except (OSError, subprocess.SubprocessError) as exc:
                    stage.detail = f"{stage.detail} Owned-process cleanup failed: {exc}"
                stage.returncode = process.poll()
            stage.duration_s = round(time.monotonic() - start, 3)
            self.save()
            renderer.finish()
        print(
            f"{stage.name}: {stage.status} ({stage.duration_s:.1f}s)"
            + (f" — {stage.detail}" if stage.detail else ""),
            flush=True,
        )
        return status

    def run_parallel(
        self,
        tasks: Sequence[tuple[int, list[str]]],
        *,
        max_workers: int,
        stagger_s: float = 0.0,
    ) -> int:
        """Execute independent stage commands concurrently with combined progress and cleanup."""

        if not tasks:
            return 0
        if max_workers <= 1 or len(tasks) == 1:
            for index, command in tasks:
                rc = self.run(index, command)
                if rc:
                    return rc
            return 0

        environment = self._build_environment()
        active_lock = threading.Lock()
        active_processes: dict[int, subprocess.Popen[bytes]] = {}
        kill_active = threading.Event()
        cancel_queued = threading.Event()
        abort_reason: list[str] = []
        next_launch_at = 0.0
        launch_lock = threading.Lock()
        suite_start = time.monotonic()

        def _reserve_launch_slot() -> bool:
            nonlocal next_launch_at
            while True:
                if cancel_queued.is_set() or kill_active.is_set():
                    return False
                if stagger_s <= 0:
                    return True
                with launch_lock:
                    now = time.monotonic()
                    if now >= next_launch_at:
                        next_launch_at = now + stagger_s
                        return True
                    delay = min(0.25, next_launch_at - now)
                time.sleep(delay)

        def _run_one(index: int, command: list[str]) -> int:
            if not _reserve_launch_slot():
                return 0
            stage = self.stages[index]
            log_path = self.directory / f"{index + 1:02d}.stdout.log"
            with self._lock:
                stage.command = command
                stage.status = "running"
                stage.log = str(log_path)
                self._save_unlocked()
            print(f"Running {stage.name}; output: {log_path}", flush=True)
            start = time.monotonic()
            deadline = start + self.timeout
            process: subprocess.Popen[bytes] | None = None
            status = 2
            try:
                with log_path.open("wb") as output:
                    process = subprocess.Popen(
                        command,
                        cwd=REPO_ROOT,
                        env=environment,
                        stdout=output,
                        stderr=subprocess.STDOUT,
                        start_new_session=os.name != "nt",
                    )
                    with active_lock:
                        active_processes[index] = process
                    with self._lock:
                        stage.pid = process.pid
                        self._save_unlocked()

                    if not isinstance(process, _REAL_POPEN):
                        stage.returncode = process.wait(timeout=self.timeout)
                    else:
                        while True:
                            if kill_active.is_set():
                                reason = abort_reason[0] if abort_reason else "interrupted"
                                if reason == "timed_out":
                                    raise subprocess.TimeoutExpired(command, self.timeout)
                                raise KeyboardInterrupt
                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                raise subprocess.TimeoutExpired(command, self.timeout)
                            try:
                                stage.returncode = process.wait(timeout=min(0.25, remaining))
                                break
                            except subprocess.TimeoutExpired:
                                continue
                    status = stage.returncode
                    stage.status = "passed" if status == 0 else "failed"
            except subprocess.TimeoutExpired:
                if not abort_reason:
                    abort_reason.append("timed_out")
                kill_active.set()
                cancel_queued.set()
                stage.status = "timed_out"
                stage.detail = f"Stage exceeded {self.timeout:g} seconds."
                status = 124
            except KeyboardInterrupt:
                if not abort_reason:
                    abort_reason.append("interrupted")
                kill_active.set()
                cancel_queued.set()
                stage.status = "interrupted"
                stage.detail = "Interrupted by the operator."
                status = 130
            except OSError as exc:
                cancel_queued.set()
                stage.status = "failed"
                stage.detail = str(exc)
            finally:
                with active_lock:
                    active_processes.pop(index, None)
                if process is not None and (
                    process.poll() is None or stage.status in {"timed_out", "interrupted"}
                ):
                    try:
                        stop_owned_process(process)
                    except (OSError, subprocess.SubprocessError) as exc:
                        stage.detail = f"{stage.detail} Owned-process cleanup failed: {exc}"
                    stage.returncode = process.poll()
                stage.duration_s = round(time.monotonic() - start, 3)
                self.save()
            return status

        renderer = _ProgressRenderer(enabled=self.show_progress)
        completed_outcomes: dict[int, int] = {}
        try:
            with ThreadPoolExecutor(max_workers=max_workers) as pool:
                future_map: dict[Future[int], int] = {
                    pool.submit(_run_one, index, command): index for index, command in tasks
                }
                pending = set(future_map)
                while pending:
                    done_set, pending = wait(pending, timeout=0.25, return_when=FIRST_COMPLETED)
                    for fut in done_set:
                        idx = future_map[fut]
                        rc = fut.result()
                        completed_outcomes[idx] = rc
                        stage = self.stages[idx]
                        if stage.status != "not_run":
                            renderer.finish()
                            print(
                                f"{stage.name}: {stage.status} ({stage.duration_s:.1f}s)"
                                + (f" — {stage.detail}" if stage.detail else ""),
                                flush=True,
                            )
                        if rc in {124, 130}:
                            if not abort_reason:
                                abort_reason.append("timed_out" if rc == 124 else "interrupted")
                            kill_active.set()
                            cancel_queued.set()
                        elif rc != 0:
                            # Stop launching queued shards while allowing running siblings to finish cleanly.
                            cancel_queued.set()

                    if pending and self.show_progress:
                        total_done = 0
                        total_known: int | None = 0
                        total_passed = 0
                        total_failed = 0
                        total_skipped = 0
                        latest_case = ""
                        running_count = 0
                        for idx, command in tasks:
                            st = self.stages[idx]
                            if st.status == "running":
                                running_count += 1
                            if st.log:
                                snap = self._read_stage_snapshot(idx, command, Path(st.log), full=True)
                                total_done += snap.done
                                total_passed += snap.passed
                                total_failed += snap.failed
                                total_skipped += snap.skipped
                                if snap.last_case and st.status == "running":
                                    latest_case = snap.last_case
                                if snap.total is None:
                                    total_known = None
                                elif total_known is not None:
                                    total_known += snap.total
                            else:
                                total_known = None
                        agg = ProgressSnapshot(
                            done=total_done,
                            total=total_known,
                            passed=total_passed,
                            failed=total_failed,
                            skipped=total_skipped,
                            last_case=latest_case,
                        )
                        label = f"[Parallel {running_count}/{max_workers} active]"
                        renderer.update(label, agg, time.monotonic() - suite_start)
        except KeyboardInterrupt:
            if not abort_reason:
                abort_reason.append("interrupted")
            kill_active.set()
            cancel_queued.set()
            with active_lock:
                procs = list(active_processes.values())
            for proc in procs:
                try:
                    stop_owned_process(proc)
                except (OSError, subprocess.SubprocessError):
                    pass
            renderer.finish()
            return 130
        finally:
            renderer.finish()

        for rc in completed_outcomes.values():
            if rc in {124, 130}:
                return rc
        for index, _ in tasks:
            rc = completed_outcomes.get(index, 0)
            if rc:
                return rc
        return 0

    def check_automation(
        self,
        index: int,
        log: Path,
        minimum_completed: int,
        *,
        required_names: tuple[str, ...] = (),
    ) -> int:
        stage = self.stages[index]
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            stage.detail = f"UE Automation log is unavailable: {exc}"
        else:
            results = re.findall(
                r"Test Completed\. Result=\{([^}]+)\} Name=\{[^}]+\} Path=\{([^}]+)\}",
                text,
            )
            completed = len(results)
            failed = [name for result, name in results if result != "Success"]
            names = [name for _, name in results]
            if "**** TEST COMPLETE. EXIT CODE: 0 ****" not in text:
                stage.detail = "UE Automation did not report successful completion."
            elif failed:
                stage.detail = "UE Automation reported unsuccessful tests: " + ", ".join(failed)
            elif len(set(names)) != completed:
                stage.detail = "UE Automation reported duplicate test completions."
            elif missing := sorted(set(required_names) - set(names)):
                stage.detail = "UE Automation did not complete required tests: " + ", ".join(missing)
            elif completed < minimum_completed:
                stage.detail = f"Only {completed} tests completed; expected at least {minimum_completed}."
            else:
                stage.detail = f"{completed} tests completed; UE log: {log}"
                self.save()
                return 0
        stage.status = "failed"
        self.save()
        print(stage.detail, flush=True)
        return 1

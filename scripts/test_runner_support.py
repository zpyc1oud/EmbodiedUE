"""Bound test subprocesses and retain one YAML report per invocation."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import yaml

from tests.e2e.support.host import host_environment, resolve_test_host
from uerl.cli.host_flags import add_host_flags
from uerl.errors import ConfigError
from uerl.host import HostProfile

REPO_ROOT = Path(__file__).resolve().parents[1]


def positive_seconds(value: str) -> float:
    """Reject zero, negative, and non-finite stage deadlines."""
    import math

    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("timeout must be a finite positive number of seconds")
    return seconds


def add_runner_flags(parser: argparse.ArgumentParser) -> None:
    add_host_flags(parser)
    parser.add_argument("--timeout", type=positive_seconds, default=1800.0, help="Timeout per stage in seconds (1800).")
    parser.add_argument(
        "--output-dir", type=Path, default=REPO_ROOT / ".scratch/test-runs",
        help="Parent for a new, unique report directory.",
    )


def selected_host(args: argparse.Namespace) -> HostProfile:
    profile = resolve_test_host(
        profile_path=args.host_profile, ue_executable=args.ue_executable, project=args.project,
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


def _stop_windows_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        # The PID comes from this Popen handle, never a process-name search.
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, timeout=20,
        )
    process.wait(timeout=10)


def stop_owned_process(process: subprocess.Popen[bytes]) -> None:
    """Stop only this invocation's child tree or POSIX process group."""
    if os.name == "nt":
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

    def __init__(self, parent: Path, stage_names: list[str], timeout: float) -> None:
        parent = parent.expanduser().resolve()
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix="validation-", dir=parent))
        self.timeout = timeout
        self.stages = [StageResult(name) for name in stage_names]
        self.host: HostProfile | None = None
        self.save()
        print(f"Test report: {self.directory / 'summary.yaml'}", flush=True)

    def save(self) -> None:
        host = None if self.host is None else {
            "profile_path": str(self.host.profile_path),
            "ue_executable": str(self.host.ue_executable),
            "project": str(self.host.project),
            "sources": self.host.sources,
        }
        report = {"timeout_s": self.timeout, "host": host, "stages": [asdict(stage) for stage in self.stages]}
        temporary = self.directory / "summary.yaml.tmp"
        temporary.write_text(yaml.safe_dump(report, sort_keys=False, allow_unicode=True), encoding="utf-8")
        temporary.replace(self.directory / "summary.yaml")

    def configure_host(self, args: argparse.Namespace, stage_index: int) -> bool:
        try:
            self.host = selected_host(args)
            missing = [path for path in (self.host.ue_executable, self.host.project) if not path.is_file()]
            if missing:
                raise ConfigError("UE test prerequisites are missing: " + ", ".join(str(path) for path in missing))
        except (ConfigError, OSError) as exc:
            self.stages[stage_index].status = "blocked"
            self.stages[stage_index].detail = str(exc)
            print(str(exc), flush=True)
            self.save()
            return False
        self.save()
        return True

    def run(self, index: int, command: list[str]) -> int:
        stage = self.stages[index]
        stage.command = command
        stage.status = "running"
        log_path = self.directory / f"{index + 1:02d}.stdout.log"
        stage.log = str(log_path)
        environment = os.environ.copy() if self.host is None else host_environment(self.host, self.directory)
        source_paths = [str(REPO_ROOT / "src"), str(REPO_ROOT)]
        if environment.get("PYTHONPATH"):
            source_paths.append(environment["PYTHONPATH"])
        environment["PYTHONPATH"] = os.pathsep.join(source_paths)
        self.save()
        print(f"Running {stage.name}; output: {log_path}", flush=True)
        start = time.monotonic()
        process: subprocess.Popen[bytes] | None = None
        status = 2
        try:
            with log_path.open("wb") as output:
                process = subprocess.Popen(
                    command, cwd=REPO_ROOT, env=environment, stdout=output, stderr=subprocess.STDOUT,
                    start_new_session=os.name != "nt",
                )
                stage.pid = process.pid
                self.save()
                stage.returncode = process.wait(timeout=self.timeout)
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
            if process is not None and stage.status in {"timed_out", "interrupted"}:
                try:
                    stop_owned_process(process)
                except (OSError, subprocess.SubprocessError) as exc:
                    stage.detail = f"{stage.detail} Owned-process cleanup failed: {exc}"
                stage.returncode = process.poll()
            stage.duration_s = round(time.monotonic() - start, 3)
            self.save()
        print(f"{stage.name}: {stage.status}" + (f" — {stage.detail}" if stage.detail else ""), flush=True)
        return status

    def check_automation(self, index: int, log: Path, minimum_completed: int) -> int:
        stage = self.stages[index]
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            stage.detail = f"UE Automation log is unavailable: {exc}"
        else:
            completed = text.count("Test Completed. Result={")
            if "**** TEST COMPLETE. EXIT CODE: 0 ****" not in text:
                stage.detail = "UE Automation did not report successful completion."
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

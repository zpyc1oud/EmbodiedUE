"""Run small real subprocesses and independent Automation completion fixtures."""
from __future__ import annotations

import argparse
import socket
import subprocess
import sys
from pathlib import Path
from unittest.mock import Mock

import pytest
import yaml

from scripts import run_all_tests, run_e2e
from scripts.test_runner_support import TestRun, _stop_windows_process, positive_seconds


@pytest.mark.parametrize("value", ["0", "-1", "nan", "inf"])
def test_timeout_must_be_positive_finite(value: str) -> None:
    with pytest.raises(argparse.ArgumentTypeError):
        positive_seconds(value)


def test_stage_output_and_summary_are_isolated(tmp_path: Path) -> None:
    first = TestRun(tmp_path, ["Python", "later"], 10)
    second = TestRun(tmp_path, ["Python"], 10)
    assert first.directory != second.directory
    assert first.run(0, [sys.executable, "-c", "print('unique evidence')"]) == 0
    assert first.stages[0].status == "passed"
    assert first.stages[1].status == "not_run"
    assert Path(str(first.stages[0].log)).read_text().strip() == "unique evidence"
    report = yaml.safe_load((first.directory / "summary.yaml").read_text())
    assert report["stages"][0]["returncode"] == 0
    assert report["stages"][1]["command"] is None
    assert yaml.safe_load((second.directory / "summary.yaml").read_text())["stages"][0]["status"] == "not_run"


def test_nonzero_exit_and_launch_error_are_failures(tmp_path: Path) -> None:
    run = TestRun(tmp_path, ["failing", "missing"], 10)
    assert run.run(0, [sys.executable, "-c", "raise SystemExit(7)"]) == 7
    assert run.stages[0].returncode == 7
    assert run.run(1, [str(tmp_path / "missing-executable")]) == 2
    assert [stage.status for stage in run.stages] == ["failed", "failed"]


def test_timeout_stops_owned_child_and_leaves_unrelated_process(tmp_path: Path) -> None:
    run = TestRun(tmp_path, ["slow", "later"], 0.5)
    unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])
    try:
        assert run.run(0, [sys.executable, "-c", "import time; print('started', flush=True); time.sleep(30)"]) == 124
        assert run.stages[0].status == "timed_out"
        assert run.stages[0].returncode is not None
        assert run.stages[1].status == "not_run"
        assert unrelated.poll() is None
        assert "started" in Path(str(run.stages[0].log)).read_text()
    finally:
        unrelated.terminate()
        unrelated.wait(timeout=10)


def test_interrupt_records_result_and_cleans_owned_process(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    process = Mock()
    process.pid = 12345
    process.wait.side_effect = KeyboardInterrupt
    process.poll.return_value = -15
    monkeypatch.setattr("scripts.test_runner_support.subprocess.Popen", Mock(return_value=process))
    cleanup = Mock()
    monkeypatch.setattr("scripts.test_runner_support.stop_owned_process", cleanup)
    run = TestRun(tmp_path, ["interrupt", "later"], 10)
    assert run.run(0, ["selected-child"]) == 130
    cleanup.assert_called_once_with(process)
    assert [stage.status for stage in run.stages] == ["interrupted", "not_run"]


@pytest.mark.parametrize("content,expected", [
    (None, 1),
    ("Test Completed. Result={Success} Name={test} Path={UERL.Unit.test}\n", 1),
    ("**** TEST COMPLETE. EXIT CODE: 0 ****\n", 1),
    ("Test Completed. Result={Success} Name={test} Path={UERL.Unit.test}\n**** TEST COMPLETE. EXIT CODE: 0 ****\n", 0),
])
def test_automation_requires_current_log_and_completion(tmp_path: Path, content: str | None, expected: int) -> None:
    run = TestRun(tmp_path, ["UE"], 10)
    run.stages[0].status = "passed"
    # A successful old log in the parent directory must not be used.
    (tmp_path / "UERLHost.log").write_text(
        "Test Completed. Result={Success} Name={test} Path={UERL.Unit.test}\n"
        "**** TEST COMPLETE. EXIT CODE: 0 ****",
    )
    log = run.directory / "01.ue.log"
    if content is not None:
        log.write_text(content)
    assert run.check_automation(0, log, 1) == expected
    assert run.stages[0].status == ("passed" if expected == 0 else "failed")


def test_e2e_command_preserves_selection() -> None:
    assert run_e2e.pytest_command("p1", "timeout") == [
        sys.executable, "-m", "pytest", "-v", "-s", "tests/e2e/test_p1_session.py", "-k", "timeout",
    ]


def test_full_runner_stops_after_python_failure(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    execute = Mock(return_value=9)
    monkeypatch.setattr(TestRun, "run", execute)
    configure = Mock()
    monkeypatch.setattr(TestRun, "configure_host", configure)
    assert run_all_tests.main(["--output-dir", str(tmp_path)]) == 9
    assert execute.call_count == 1
    configure.assert_not_called()


def test_windows_cleanup_targets_only_owned_pid(monkeypatch: pytest.MonkeyPatch) -> None:
    process = Mock()
    process.pid = 12345
    process.poll.return_value = None
    command = Mock()
    monkeypatch.setattr("scripts.test_runner_support.subprocess.run", command)
    _stop_windows_process(process)
    assert command.call_args.args[0] == ["taskkill", "/PID", "12345", "/T", "/F"]
    assert command.call_args.kwargs["check"] is True
    process.wait.assert_called_once_with(timeout=10)


def test_windows_cleanup_does_not_target_an_exited_pid(monkeypatch: pytest.MonkeyPatch) -> None:
    process = Mock()
    process.pid = 12345
    process.poll.return_value = 0
    command = Mock()
    monkeypatch.setattr("scripts.test_runner_support.subprocess.run", command)
    _stop_windows_process(process)
    command.assert_not_called()


def test_cleanup_failure_is_visible(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    process = Mock()
    process.pid = 12345
    process.wait.side_effect = subprocess.TimeoutExpired("child", 1)
    process.poll.return_value = None
    monkeypatch.setattr("scripts.test_runner_support.subprocess.Popen", Mock(return_value=process))
    monkeypatch.setattr("scripts.test_runner_support.stop_owned_process", Mock(side_effect=PermissionError("denied")))
    run = TestRun(tmp_path, ["slow"], 1)
    assert run.run(0, ["owned-child"]) == 124
    assert run.stages[0].status == "timed_out"
    assert "cleanup failed" in str(run.stages[0].detail)


def test_full_runner_passes_paths_and_uses_distinct_automation_logs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    executable = tmp_path / "editor.exe"
    project = tmp_path / "host.uproject"
    executable.touch()
    project.touch()
    monkeypatch.delenv("UERL_HOST_PROFILE", raising=False)
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    commands: list[list[str]] = []

    def execute(self: TestRun, index: int, command: list[str]) -> int:
        commands.append(command)
        self.stages[index].status = "passed"
        if index in {1, 2}:
            log_argument = next(arg for arg in command if arg.startswith("-abslog="))
            minimum = 142 if index == 1 else 13
            Path(log_argument.removeprefix("-abslog=")).write_text(
                "".join(f"Test Completed. Result={{Success}} Name={{test{case}}} Path={{UERL.Unit.test{case}}}\n"
                        for case in range(minimum)) + "**** TEST COMPLETE. EXIT CODE: 0 ****\n",
            )
        return 0

    monkeypatch.setattr(TestRun, "run", execute)
    assert run_all_tests.main([
        "--output-dir", str(tmp_path / "reports"), "--ue-executable", str(executable), "--project", str(project),
    ]) == 0
    assert len(commands) == 4
    assert commands[1][:2] == commands[2][:2] == [str(executable), str(project)]
    first_log = next(arg for arg in commands[1] if arg.startswith("-abslog="))
    second_log = next(arg for arg in commands[2] if arg.startswith("-abslog="))
    assert first_log != second_log
    assert commands[-1] == [sys.executable, "-m", "pytest", "-v", "-s", "tests/e2e"]


@pytest.mark.skipif(sys.platform != "linux", reason="Inspect the owned descendant through Linux /proc")
def test_timeout_stops_descendant_that_ignores_termination(tmp_path: Path) -> None:
    import time

    descendant = "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)"
    code = (
        "import subprocess,sys,time; "
        f"child=subprocess.Popen([sys.executable, '-c', {descendant!r}]); "
        "print(child.pid, flush=True); time.sleep(30)"
    )
    run = TestRun(tmp_path, ["tree"], 2)
    assert run.run(0, [sys.executable, "-c", code]) == 124
    child_pid = int(Path(str(run.stages[0].log)).read_text().strip())
    state_file = Path(f"/proc/{child_pid}/stat")
    deadline = time.monotonic() + 2
    while state_file.exists() and time.monotonic() < deadline:
        if state_file.read_text().split(") ", 1)[1].split()[0] == "Z":
            break  # Exited and awaiting reaping by the container's init process.
        time.sleep(0.02)
    assert not state_file.exists() or state_file.read_text().split(") ", 1)[1].split()[0] == "Z"


def test_report_write_failure_after_spawn_still_cleans_child(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    run = TestRun(tmp_path, ["report failure"], 10)
    process = Mock()
    process.pid = 12345
    process.poll.return_value = None
    monkeypatch.setattr("scripts.test_runner_support.subprocess.Popen", Mock(return_value=process))
    monkeypatch.setattr(run, "save", Mock(side_effect=[None, OSError("disk full"), None]))
    cleanup = Mock()
    monkeypatch.setattr("scripts.test_runner_support.stop_owned_process", cleanup)
    assert run.run(0, ["owned-child"]) == 2
    assert run.stages[0].status == "failed"
    assert run.stages[0].detail == "disk full"
    cleanup.assert_called_once_with(process)


@pytest.mark.parametrize("results", [
    "Test Completed. Result={Fail} Name={failed} Path={UERL.Unit.failed}\n",
    "Test Completed. Result={Success} Name={same} Path={UERL.Unit.same}\n" * 2,
])
def test_automation_rejects_failure_or_duplicate_even_with_exit_zero(tmp_path: Path, results: str) -> None:
    run = TestRun(tmp_path, ["UE"], 10)
    log = run.directory / "ue.log"
    log.write_text(results + "**** TEST COMPLETE. EXIT CODE: 0 ****\n", encoding="utf-8")
    assert run.check_automation(0, log, 1) == 1
    assert run.stages[0].status == "failed"


def test_raw_connect_failure_reclaims_launched_worker(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    from tests.e2e.support import socket_negative

    process = Mock()
    stopped = Mock()
    monkeypatch.setattr(socket_negative, "_launch", Mock(return_value=process))
    monkeypatch.setattr(socket_negative, "worker_log_path", lambda name: str(tmp_path / name))
    monkeypatch.setattr(socket_negative, "_connect_raw", Mock(side_effect=TimeoutError("connect deadline")))
    monkeypatch.setattr(socket_negative, "_stop_process", stopped)
    with pytest.raises(TimeoutError, match="connect deadline"):
        socket_negative.run_timeout_and_half_close()
    stopped.assert_called_once_with(process)


def test_raw_connect_uses_remaining_startup_budget(monkeypatch: pytest.MonkeyPatch) -> None:
    from tests.e2e.support import socket_negative

    connect = Mock(return_value=Mock())
    monkeypatch.setattr(socket, "create_connection", connect)
    socket_negative._connect_raw(1234, timeout_s=7)
    assert 6.9 < connect.call_args.kwargs["timeout"] <= 7


@pytest.mark.parametrize("primary_failure", [False, True])
def test_worker_cleanup_attempts_all_resources_and_preserves_primary(
    monkeypatch: pytest.MonkeyPatch, primary_failure: bool,
) -> None:
    from tests.e2e.support import worker_runner

    env = Mock()
    session = Mock()
    process = Mock()
    env.close.side_effect = OSError("env close")
    session.close.side_effect = OSError("session close")
    process.poll.return_value = None
    stopped = Mock(return_value=0)
    monkeypatch.setattr(worker_runner, "_stop_process", stopped)
    if primary_failure:
        primary = RuntimeError("original test failure")
        with pytest.raises(RuntimeError) as raised:
            try:
                raise primary
            finally:
                worker_runner._cleanup_run(env, session, process, reason="test", terminate_host=True)
        assert raised.value is primary
        assert len(primary.__notes__) == 2
    else:
        with pytest.raises(OSError, match="env close"):
            worker_runner._cleanup_run(env, session, process, reason="test", terminate_host=True)
    session.close.assert_called_once_with("test")
    process.terminate.assert_called_once()
    stopped.assert_called_once_with(process)


@pytest.mark.skipif(sys.platform != "win32", reason="Windows owned process tree cleanup")
def test_training_timeout_reclaims_real_windows_descendant(tmp_path: Path) -> None:
    if sys.platform != "win32":
        pytest.skip("Windows owned process tree cleanup")
    import ctypes
    import time

    from tests.e2e.support.worker_runner import run_owned_command

    pid_file = tmp_path / "owned-child.pid"
    code = (
        "import subprocess,sys,time; "
        "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
        f"open({str(pid_file)!r},'w').write(str(child.pid)); "
        "time.sleep(30)"
    )
    with pytest.raises(subprocess.TimeoutExpired):
        run_owned_command([sys.executable, "-c", code], cwd=tmp_path, timeout=2)
    child_pid = int(pid_file.read_text())
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    deadline = time.monotonic() + 3
    while True:
        handle = kernel.OpenProcess(0x1000, False, child_pid)
        if not handle:
            break
        try:
            exit_code = ctypes.c_ulong()
            assert kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code))
            if exit_code.value != 259:
                break
        finally:
            kernel.CloseHandle(handle)
        assert time.monotonic() < deadline, f"owned descendant {child_pid} survived timeout"
        time.sleep(0.02)


def test_phase_helper_rejects_unknown_mode_without_launch(monkeypatch: pytest.MonkeyPatch) -> None:
    from tests.e2e.support import phase1

    launch = Mock(side_effect=AssertionError("invalid selection reached UE"))
    monkeypatch.setattr(phase1, "run_negative_gate", launch)
    with pytest.raises(SystemExit) as error:
        phase1.main(["typo"])
    assert error.value.code == 2
    launch.assert_not_called()

"""Run the repository's complete validation: Python, UE unit, and UE E2E suites."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# PIEAttach is run by the E2E suite, which supplies its required loopback port
# and Python client. Keep the remaining Automation filters complete for the
# currently registered Unit and Integration families. Run the added terrain
# group in a fresh process so preview-scene physics is independent of prior
# Automation state.
UE_AUTOMATION_GROUPS: tuple[tuple[str, str, int], ...] = (
    (
        "unit and core integration",
        "UERL.Unit+"
        "UERL.Integration.Worker.SlotCollision+"
        "UERL.Integration.Worker.VariableDt+"
        "UERL.Integration.Worker.SharedWorldCollision+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003+"
        "UERL.Integration.Policy.Contact+"
        "UERL.Integration.Policy.Ground+"
        "UERL.Integration.Policy.Clock+"
        "UERL.Integration.Policy.Controller+"
        "UERL.Integration.Policy.Component",
        135,
    ),
    (
        "additional robot, environment-pool, and terrain integration",
        "UERL.Integration.Robot.GenericDrive+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_001+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_002+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_004+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_005+"
        "UERL.Integration.Robot.TopologyReflector+"
        "UERL.Integration.Worker.EnvironmentPool+"
        "UERL.Integration.Worker.Terrain",
        13,
    ),
)


def _run(command: list[str]) -> int:
    return subprocess.run(command, cwd=REPO_ROOT, check=False).returncode


def _run_ue_automation() -> int:
    """Run every Unit and Integration Automation group before external E2E."""
    sys.path.insert(0, str(REPO_ROOT))
    sys.path.insert(0, str(REPO_ROOT / "src"))
    from tests.e2e.support.worker_runner import UE_CMD, UPROJECT

    missing = [path for path in (Path(UE_CMD), Path(UPROJECT)) if not path.is_file()]
    if missing:
        print("UE unit test prerequisites are missing:", file=sys.stderr)
        for path in missing:
            print(f"  {path}", file=sys.stderr)
        return 2

    log_path = REPO_ROOT / "engine" / "Saved" / "Logs" / "UERLHost.log"
    for group_name, filters, minimum_completed in UE_AUTOMATION_GROUPS:
        print(f"Running UE Automation group: {group_name}", flush=True)
        status = _run([
            UE_CMD,
            UPROJECT,
            "/Engine/Maps/Entry",
            f"-ExecCmds=Automation RunTests {filters};Quit",
            "-unattended",
            "-nullrhi",
            "-nosound",
            "-NoSplash",
        ])
        if status:
            return status

        if not log_path.is_file():
            print(f"UE Automation log is missing: {log_path}", file=sys.stderr)
            return 2
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        if "**** TEST COMPLETE. EXIT CODE: 0 ****" not in log_text:
            print(
                f"UE Automation group '{group_name}' did not report a successful completion.",
                file=sys.stderr,
            )
            return 1
        completed = log_text.count("Test Completed. Result={")
        if completed < minimum_completed:
            print(
                f"UE Automation group '{group_name}' selected only {completed} tests; "
                f"expected at least {minimum_completed}.",
                file=sys.stderr,
            )
            return 1
        print(f"UE Automation group '{group_name}': {completed} tests completed.", flush=True)
    return 0


def main() -> int:
    """Run fast Python tests, UE unit tests, and all real E2E suites."""
    fast_status = _run([sys.executable, "-m", "pytest", "-q"])
    if fast_status:
        return fast_status
    ue_status = _run_ue_automation()
    if ue_status:
        return ue_status
    return _run([sys.executable, str(REPO_ROOT / "scripts" / "run_e2e.py"), "--suite", "all"])


if __name__ == "__main__":
    raise SystemExit(main())

"""Run the repository's real external-process E2E suites through one CLI entry point."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import cast

REPO_ROOT = Path(__file__).resolve().parents[1]
PYTEST_TARGETS = {
    "all": ("tests/e2e",),
    "p1": ("tests/e2e/test_p1_session.py",),
    "p2": ("tests/e2e/test_p2_direct_env.py",),
    "ue": (
        "tests/e2e/test_cartpole_worker.py",
        "tests/e2e/test_phantomx_worker.py",
        "tests/e2e/test_event_manager.py",
    ),
}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run real external-process end-to-end tests.")
    _ = parser.add_argument(
        "--suite",
        choices=tuple(PYTEST_TARGETS),
        default="all",
        help="E2E suite to run (default: all).",
    )
    _ = parser.add_argument(
        "-k",
        "--keyword",
        dest="keyword",
        help="Only run tests matching a pytest expression.",
    )
    return parser


def _check_ue_prerequisites() -> int:
    """Reject an E2E invocation when its Unreal runtime is absent."""
    sys.path.insert(0, str(REPO_ROOT))
    sys.path.insert(0, str(REPO_ROOT / "src"))
    from tests.e2e.support.worker_runner import UE_CMD, UPROJECT

    missing = [path for path in (Path(UE_CMD), Path(UPROJECT)) if not path.is_file()]
    if not missing:
        return 0

    print("UE E2E prerequisites are missing:", file=sys.stderr)
    for path in missing:
        print(f"  {path}", file=sys.stderr)
    return 2


def _check_prerequisites(suite: str) -> int:
    """Reject a selected E2E invocation before pytest starts if its runtime is absent."""
    if suite in {"all", "p1", "p2", "ue"}:
        return _check_ue_prerequisites()
    return 0


def main(argv: list[str] | None = None) -> int:
    """Parse the E2E selection and delegate to pytest."""

    args = _parser().parse_args(argv)
    suite = cast(str, args.suite)
    keyword = cast(str | None, args.keyword)
    prerequisite_status = _check_prerequisites(suite)
    if prerequisite_status:
        return prerequisite_status

    command = [
        sys.executable,
        "-m",
        "pytest",
        "-v",
        "-s",
        *PYTEST_TARGETS[suite],
    ]
    if keyword:
        command.extend(["-k", keyword])
    environment = os.environ.copy()
    source_path = str(REPO_ROOT / "src")
    existing_pythonpath = environment.get("PYTHONPATH")
    environment["PYTHONPATH"] = os.pathsep.join(
        [source_path, existing_pythonpath] if existing_pythonpath else [source_path]
    )
    return subprocess.run(command, cwd=REPO_ROOT, env=environment, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())

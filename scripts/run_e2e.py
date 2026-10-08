"""Run the repository's real external-process E2E suites through one CLI entry point."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
PYTEST_TARGETS = {
    "all": ("tests/e2e",),
    "p1": ("tests/e2e/test_p1_session.py",),
    "p2": ("tests/e2e/test_p2_direct_env.py",),
    "ue": (
        "tests/e2e/test_cartpole_worker.py",
        "tests/e2e/test_phantomx_worker.py",
        "tests/e2e/test_catch_worker.py",
        "tests/e2e/test_event_manager.py",
    ),
}


# Support direct script execution without an editable package install.
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "src"))



def _parser() -> argparse.ArgumentParser:
    from scripts.test_runner_support import add_runner_flags

    parser = argparse.ArgumentParser(description="Run real external-process end-to-end tests.")
    parser.add_argument("--suite", choices=tuple(PYTEST_TARGETS), default="all")
    parser.add_argument("-k", "--keyword", dest="keyword", help="Select a pytest expression.")
    add_runner_flags(parser)
    return parser


def pytest_command(suite: str, keyword: str | None = None) -> list[str]:
    command = [sys.executable, "-m", "pytest", "-v", "-s", *PYTEST_TARGETS[suite]]
    if keyword:
        command.extend(["-k", keyword])
    return command


def main(argv: list[str] | None = None) -> int:
    from scripts.test_runner_support import TestRun

    args = _parser().parse_args(argv)
    run = TestRun(args.output_dir, ["UE E2E"], args.timeout)
    if not run.configure_host(args, 0):
        return 2
    return run.run(0, pytest_command(args.suite, args.keyword))


if __name__ == "__main__":
    raise SystemExit(main())

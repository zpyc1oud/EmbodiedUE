"""Thin script wrapper around ``uerl diagnose training-trace``."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from uerl.cli.diagnose import run_training_trace  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    return run_training_trace(argv)


if __name__ == "__main__":
    raise SystemExit(main())

"""Compare saved Task and Unreal Engine policy-frame YAML traces."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "src"))

from uerl.training.policy_trace import compare_policy_traces  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("task_trace", type=Path, help="YAML written by `uerl play --trace`.")
    parser.add_argument("ue_trace", type=Path, help="YAML written by the UE host trace recorder.")
    args = parser.parse_args(argv)
    try:
        report = compare_policy_traces(args.task_trace, args.ue_trace)
    except (OSError, TypeError, KeyError, IndexError, ValueError) as exc:
        print(f"Trace comparison failed: {exc}", file=sys.stderr)
        return 2
    print(yaml.safe_dump(report, sort_keys=False, allow_unicode=True), end="")
    return 0 if report["status"] == "comparable" else 2


if __name__ == "__main__":
    raise SystemExit(main())

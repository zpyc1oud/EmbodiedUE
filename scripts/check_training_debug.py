"""Check a bounded all-Slot training capture without starting Unreal Engine."""

import argparse
from pathlib import Path

import yaml

from uerl.training.debug_audit import audit_training_trace


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    print(yaml.safe_dump(audit_training_trace(args.trace), sort_keys=False), end="")


if __name__ == "__main__":
    main()

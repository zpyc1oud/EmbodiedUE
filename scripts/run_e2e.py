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
        "tests/e2e/test_phantomx_physical_response.py",
        "tests/e2e/test_event_manager.py",
    ),
}

# Coarse runtime/resource weights so greedy sharding balances slow suites and keeps
# 64-slot procedural terrain workers serialized within one bucket (~3 GB peak RAM each).
_HEAVY_TERRAIN_FILES = frozenset({"test_event_manager.py", "test_terrain_curriculum.py"})
_E2E_WEIGHT_HINTS: dict[str, int] = {
    "test_phantomx_variants_composed.py": 300,
    "test_cartpole_worker.py": 250,
    "test_phantomx_worker.py": 180,
    "test_phantomx_physical_response.py": 180,
    "test_terrain_curriculum.py": 120,
    "test_fresh_policy_game.py": 120,
    "test_event_manager.py": 80,
    "test_p2_direct_env.py": 60,
    "test_p1_session.py": 40,
}


# Support direct script execution without an editable package install.
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "src"))


def _expand_e2e_targets(suite: str) -> list[str]:
    files: list[str] = []
    for raw in PYTEST_TARGETS[suite]:
        candidate = REPO_ROOT / raw
        if candidate.is_dir():
            for child in sorted(candidate.glob("test_*.py")):
                files.append(child.relative_to(REPO_ROOT).as_posix())
        else:
            files.append(raw)
    return files


def plan_e2e_shards(suite: str, jobs: int) -> list[list[str]]:
    """Partition E2E targets into up to ``jobs`` resource-safe shards."""

    if jobs <= 1:
        return [list(PYTEST_TARGETS[suite])]
    files = _expand_e2e_targets(suite)
    if len(files) <= 1:
        return [files]

    terrain_group = [path for path in files if Path(path).name in _HEAVY_TERRAIN_FILES]
    units: list[tuple[int, list[str]]] = []
    if terrain_group:
        weight = sum(_E2E_WEIGHT_HINTS.get(Path(path).name, 80) for path in terrain_group)
        units.append((weight, terrain_group))
    for path in files:
        if Path(path).name in _HEAVY_TERRAIN_FILES:
            continue
        units.append((_E2E_WEIGHT_HINTS.get(Path(path).name, 80), [path]))

    shard_count = min(jobs, len(units))
    buckets: list[list[str]] = [[] for _ in range(shard_count)]
    loads = [0] * shard_count
    # Place heaviest independent units first so the slowest bucket determines makespan cleanly.
    for weight, unit_files in sorted(units, key=lambda item: (-item[0], item[1][0])):
        target_idx = min(range(shard_count), key=lambda idx: (loads[idx], len(buckets[idx]), idx))
        buckets[target_idx].extend(unit_files)
        loads[target_idx] += weight
    return [sorted(bucket) for bucket in buckets if bucket]


def _parser() -> argparse.ArgumentParser:
    from scripts.test_runner_support import add_runner_flags, positive_int

    parser = argparse.ArgumentParser(description="Run real external-process end-to-end tests.")
    parser.add_argument("--suite", choices=tuple(PYTEST_TARGETS), default="all")
    parser.add_argument("-k", "--keyword", dest="keyword", help="Select a pytest expression.")
    parser.add_argument(
        "-j",
        "--jobs",
        type=positive_int,
        default=1,
        help="Run up to N E2E shards in parallel processes (default: 1 serial run).",
    )
    add_runner_flags(parser)
    return parser


def pytest_command(suite: str, keyword: str | None = None) -> list[str]:
    command = [sys.executable, "-m", "pytest", "-v", "-s", *PYTEST_TARGETS[suite]]
    if keyword:
        command.extend(["-k", keyword])
    return command


def pytest_shard_command(
    targets: list[str],
    keyword: str | None,
    *,
    basetemp: Path,
) -> list[str]:
    command = [
        sys.executable,
        "-m",
        "pytest",
        "-p",
        "no:cacheprovider",
        "-v",
        "-s",
        f"--basetemp={basetemp}",
        *targets,
    ]
    if keyword:
        command.extend(["-k", keyword])
    return command


def main(argv: list[str] | None = None) -> int:
    from scripts.test_runner_support import TestRun

    args = _parser().parse_args(argv)
    shards = plan_e2e_shards(args.suite, args.jobs)
    if len(shards) <= 1:
        run = TestRun(
            args.output_dir,
            ["UE E2E"],
            args.timeout,
            show_progress=not args.no_progress,
        )
        if not run.configure_host(args, 0):
            return 2
        return run.run(0, pytest_command(args.suite, args.keyword))

    stage_names = [f"UE E2E [{idx + 1}/{len(shards)}]" for idx in range(len(shards))]
    run = TestRun(
        args.output_dir,
        stage_names,
        args.timeout,
        show_progress=not args.no_progress,
    )
    if not run.configure_host(args, 0):
        return 2
    tasks = [
        (
            idx,
            pytest_shard_command(
                shard,
                args.keyword,
                basetemp=run.directory / f"tmp-e2e-{idx + 1}",
            ),
        )
        for idx, shard in enumerate(shards)
    ]
    return run.run_parallel(tasks, max_workers=len(shards), stagger_s=2.0)


if __name__ == "__main__":
    raise SystemExit(main())

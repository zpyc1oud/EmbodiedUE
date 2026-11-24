"""Run the repository's complete validation: Python, UE unit, and UE E2E suites."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

PYTHON_TEST_ROOTS: tuple[str, ...] = (
    "tests/python/unit",
    "tests/python/integration",
    "tests/protocol/unit",
    "tests/protocol/integration",
    "tests/tooling",
    "tests/parity",
)

# PIEAttach is run by the E2E suite, which supplies its required loopback port
# and Python client. Keep the remaining Automation filters complete for the
# currently registered Unit and Integration families. Run the added terrain
# group in a fresh process so preview-scene physics is independent of prior
# Automation state.
UE_AUTOMATION_GROUPS: tuple[tuple[str, str], ...] = (
    (
        "unit and core integration",
        "UERL.Unit+"
        "UERL.Integration.Worker.SlotCollision+"
        "UERL.Integration.Worker.SharedWorldCollision+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003+"
        "UERL.Integration.Policy.Contact+"
        "UERL.Integration.Policy.Ground+"
        "UERL.Integration.Policy.Clock+"
        "UERL.Integration.Policy.Controller+"
        "UERL.Integration.Policy.DeclaredInputs+"
        "UERL.Integration.Policy.Component",
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
    ),
    (
        "quantitative physics response",
        "UERL.Integration.PhysicsResponse",
    ),
)

# These cases need a client or an artifact produced by their owning E2E test.
E2E_AUTOMATION_CASES = {
    "UERL.Integration.Worker.PIEAttach",
    "UERL.E2E.Policy.FreshExport",
}


def selected_automation_cases() -> tuple[tuple[str, ...], ...]:
    """Require the checked-in registrations to have exactly one execution owner."""
    sources = (
        REPO_ROOT / "engine/Plugins/UERLEngine/Source",
        REPO_ROOT / "engine/Plugins/UERLTraining/Source",
    )
    names = [
        name
        for source in sources
        for path in source.rglob("*.cpp")
        for name in re.findall(
            r'IMPLEMENT_\w+_AUTOMATION_TEST\s*\(\s*\w+\s*,\s*"([^"]+)"',
            path.read_text(encoding="utf-8-sig"),
        )
    ]
    if not names or len(set(names)) != len(names):
        raise ValueError("native Automation registrations are empty or duplicated")
    selected = tuple(
        tuple(
            sorted(
                name
                for name in names
                if any(part in name for part in filters.split("+"))
            )
        )
        for _, filters in UE_AUTOMATION_GROUPS
    )
    for name in names:
        owners = sum(name in group for group in selected) + int(name in E2E_AUTOMATION_CASES)
        if owners != 1:
            raise ValueError(f"Automation case must have exactly one execution owner: {name}")
    if not all(selected) or not E2E_AUTOMATION_CASES.issubset(names):
        raise ValueError("Automation execution group or E2E registration is missing")
    return selected


PHYSICS_RESPONSE_CASES: tuple[str, ...] = selected_automation_cases()[2]


def discover_python_shards(jobs: int) -> list[list[str]]:
    """Partition non-UE pytest files into up to ``jobs`` balanced shards by byte size."""

    if jobs <= 1:
        return [[]]
    files: list[tuple[int, str]] = []
    for root_rel in PYTHON_TEST_ROOTS:
        root = REPO_ROOT / root_rel
        if not root.is_dir():
            continue
        for child in sorted(root.rglob("test_*.py")):
            try:
                weight = max(1, child.stat().st_size)
            except OSError:
                weight = 1
            files.append((weight, child.relative_to(REPO_ROOT).as_posix()))
    if len(files) <= 1:
        return [[path for _, path in files]]

    shard_count = min(jobs, len(files))
    buckets: list[list[str]] = [[] for _ in range(shard_count)]
    loads = [0] * shard_count
    for weight, path in sorted(files, key=lambda item: (-item[0], item[1])):
        target_idx = min(range(shard_count), key=lambda idx: (loads[idx], len(buckets[idx]), idx))
        buckets[target_idx].append(path)
        loads[target_idx] += weight
    return [sorted(bucket) for bucket in buckets if bucket]


sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "src"))


def _resolve_jobs(args: argparse.Namespace) -> tuple[int, int, int]:
    # Measured on 16 GB Windows host: E2E (85% of total time) speeds up ~1.8x at 2 workers,
    # while parallelizing Python (torch+cu128 DLL load) or 3 UE Automation Editors adds contention.
    default_py = 1
    default_auto = 1
    default_e2e = 2 if args.fast else 1
    return (
        args.python_jobs if args.python_jobs is not None else default_py,
        args.automation_jobs if args.automation_jobs is not None else default_auto,
        args.e2e_jobs if args.e2e_jobs is not None else default_e2e,
    )


def main(argv: list[str] | None = None) -> int:
    from scripts.run_e2e import plan_e2e_shards, pytest_command, pytest_shard_command
    from scripts.test_runner_support import TestRun, add_runner_flags, positive_int

    parser = argparse.ArgumentParser(description=__doc__)
    add_runner_flags(parser)
    parser.add_argument(
        "--python-jobs",
        type=positive_int,
        default=None,
        help="Run non-UE pytest across N parallel shards (default: 1).",
    )
    parser.add_argument(
        "--automation-jobs",
        type=positive_int,
        default=None,
        help="Run UE Automation groups across up to N parallel Editor processes (default: 1).",
    )
    parser.add_argument(
        "--e2e-jobs",
        type=positive_int,
        default=None,
        help="Run UE E2E test files across N parallel worker shards (default: 1, or 2 with --fast).",
    )
    parser.add_argument(
        "--fast",
        action="store_true",
        help="Preset parallel test execution (--e2e-jobs 2; keeps Python and UE Automation serial for 16 GB hosts).",
    )
    args = parser.parse_args(argv)
    python_jobs, automation_jobs, e2e_jobs = _resolve_jobs(args)
    required_cases = selected_automation_cases()

    py_shards = discover_python_shards(python_jobs)
    e2e_shards = plan_e2e_shards("all", e2e_jobs)

    py_names = (
        ["Python"]
        if len(py_shards) <= 1
        else [f"Python [{idx + 1}/{len(py_shards)}]" for idx in range(len(py_shards))]
    )
    auto_names = [name for name, _ in UE_AUTOMATION_GROUPS]
    e2e_names = (
        ["UE E2E"]
        if len(e2e_shards) <= 1
        else [f"UE E2E [{idx + 1}/{len(e2e_shards)}]" for idx in range(len(e2e_shards))]
    )
    names = [*py_names, *auto_names, *e2e_names]

    run = TestRun(
        args.output_dir,
        names,
        args.timeout,
        show_progress=not args.no_progress,
    )

    # Stage family 0: Python unit / protocol / integration / parity / tooling.
    if len(py_shards) <= 1:
        status = run.run(0, [sys.executable, "-m", "pytest", "-q"])
    else:
        py_tasks = [
            (
                idx,
                [
                    sys.executable,
                    "-m",
                    "pytest",
                    "-p",
                    "no:cacheprovider",
                    f"--basetemp={run.directory / f'tmp-py-{idx + 1}'}",
                    "-q",
                    *shard,
                ],
            )
            for idx, shard in enumerate(py_shards)
        ]
        status = run.run_parallel(py_tasks, max_workers=len(py_shards), stagger_s=0.0)
    if status:
        return status

    auto_start = len(py_names)
    if not run.configure_host(args, auto_start):
        return 2
    assert run.host is not None

    # Stage family 1: UE Automation groups (sequential by default or up to N concurrent processes).
    auto_commands: list[tuple[int, list[str], Path, tuple[str, ...]]] = []
    for offset, (_, filters) in enumerate(UE_AUTOMATION_GROUPS):
        stage_index = auto_start + offset
        expected_names = required_cases[offset]
        log = run.directory / f"{stage_index + 1:02d}.ue.log"
        command = [
            str(run.host.ue_executable),
            str(run.host.project),
            "/Engine/Maps/Entry",
            f"-ExecCmds=Automation RunTests {filters};Quit",
            f"-abslog={log}",
            "-unattended",
            "-nullrhi",
            "-nosound",
            "-NoSplash",
            "-notraceserver",
            "-trace=none",
            "-traceautostart=false",
        ]
        run.set_stage_progress(
            stage_index,
            total=len(expected_names),
            log_path=log,
            kind="automation",
        )
        auto_commands.append((stage_index, command, log, expected_names))

    if automation_jobs <= 1:
        for stage_index, command, log, expected_names in auto_commands:
            status = run.run(stage_index, command)
            if status:
                return status
            status = run.check_automation(
                stage_index,
                log,
                len(expected_names),
                required_names=expected_names,
            )
            if status:
                return status
    else:
        auto_tasks = [(stage_index, command) for stage_index, command, _, _ in auto_commands]
        status = run.run_parallel(
            auto_tasks,
            max_workers=min(automation_jobs, len(auto_tasks)),
            stagger_s=1.5,
        )
        check_failed = False
        for stage_index, _, log, expected_names in auto_commands:
            stage = run.stages[stage_index]
            if stage.status in {"passed", "failed"} and run.check_automation(
                stage_index,
                log,
                len(expected_names),
                required_names=expected_names,
            ):
                check_failed = True
        if status:
            return status
        if check_failed:
            return 1

    # Stage family 2: Real UE E2E.
    e2e_start = auto_start + len(auto_names)
    if len(e2e_shards) <= 1:
        return run.run(e2e_start, pytest_command("all"))
    e2e_tasks = [
        (
            e2e_start + idx,
            pytest_shard_command(
                shard,
                None,
                basetemp=run.directory / f"tmp-e2e-{idx + 1}",
            ),
        )
        for idx, shard in enumerate(e2e_shards)
    ]
    return run.run_parallel(e2e_tasks, max_workers=len(e2e_shards), stagger_s=2.0)


if __name__ == "__main__":
    raise SystemExit(main())

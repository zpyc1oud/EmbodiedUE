"""Measure bounded zero-action runtime cost without policy inference or training."""

from __future__ import annotations

import argparse
import math
import os
import statistics
import sys
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from time import perf_counter
from typing import cast

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "src"))

import torch  # noqa: E402
import yaml  # noqa: E402

from uerl import UERLDirectEnv, UERLSession, UERLSessionAdapter  # noqa: E402
from uerl.cli.host_flags import add_host_flags, resolve_host_flags  # noqa: E402
from uerl.core.config.manifest import capture_git_identity  # noqa: E402
from uerl.core.direct.profiling import StageProfiler  # noqa: E402
from uerl.runtime.session import WorkerProcessController  # noqa: E402
from uerl.tasks.registry import create_default_registry  # noqa: E402
from uerl.training import build_launch_overrides, build_run_config  # noqa: E402


def summarize(
    latencies_s: Sequence[float],
    *,
    wall_seconds: float,
    simulated_seconds: float,
    slots: int,
) -> dict[str, object]:
    """Use complete wall time for throughput and per-call time for latency percentiles."""
    if not latencies_s or not math.isfinite(wall_seconds) or wall_seconds <= 0 or slots < 1:
        raise ValueError("benchmark needs measured steps, positive wall time and Slots")
    if any(not math.isfinite(value) or value < 0 for value in latencies_s):
        raise ValueError("step latencies must be finite and non-negative")
    if not math.isfinite(simulated_seconds) or simulated_seconds <= 0:
        raise ValueError("benchmark needs positive completed physical time")
    ordered = sorted(latencies_s)
    rank = 0.95 * (len(ordered) - 1)
    lower = math.floor(rank)
    upper = math.ceil(rank)
    p95 = ordered[lower] + (ordered[upper] - ordered[lower]) * (rank - lower)
    count = len(ordered)
    return {
        "measured_steps": count,
        "slots": slots,
        "slot_transitions": count * slots,
        "wall_seconds": wall_seconds,
        "physical_seconds_per_slot": simulated_seconds,
        "control_steps_per_wall_second": count / wall_seconds,
        "slot_transitions_per_wall_second": count * slots / wall_seconds,
        "physical_seconds_per_wall_second": simulated_seconds / wall_seconds,
        "step_latency_s": {"mean": statistics.fmean(ordered), "median": statistics.median(ordered), "p95": p95},
    }


def measure(
    env: UERLDirectEnv,
    *,
    steps: int,
    warmup_steps: int,
    clock: Callable[[], float] = perf_counter,
) -> dict[str, object]:
    """Measure DirectEnv transitions; any invalid/faulted Slot aborts the benchmark."""
    if steps < 1 or warmup_steps < 0:
        raise ValueError("steps must be positive and warmup_steps non-negative")
    actions = torch.zeros((env.num_envs, env.task.num_actions), device=env.device)
    latencies: list[float] = []
    stage_seconds: dict[str, float] = {}
    physical_seconds = 0.0
    completed_episodes = 0

    def checked_step() -> tuple[torch.Tensor, torch.Tensor, Mapping[str, object]]:
        observations, rewards, terminated, truncated, info = env.step(actions)
        valid = cast(torch.Tensor, info["terminal_observation_valid"])
        faults = cast(torch.Tensor, info["slot_fault_code"])
        if not bool(valid.all()) or bool(faults.ne(0).any()):
            raise RuntimeError("runtime benchmark encountered an invalid or faulted Slot")
        if not bool(torch.isfinite(rewards).all()) or any(
            not bool(torch.isfinite(value).all()) for value in observations.values()
        ):
            raise RuntimeError("runtime benchmark received a non-finite observation or reward")
        return terminated, truncated, info

    for _ in range(warmup_steps):
        checked_step()
    start = clock()
    for _ in range(steps):
        step_start = clock()
        terminated, truncated, info = checked_step()
        latencies.append(clock() - step_start)
        dt = float(cast(float, info["transition_dt"]))
        if not math.isfinite(dt) or dt <= 0:
            raise RuntimeError("runtime benchmark received an invalid completed interval")
        physical_seconds += dt
        completed_episodes += int((terminated | truncated).sum().item())
        metrics = cast(Mapping[str, object], info["episode_metrics"])
        for name, value in metrics.items():
            if name.startswith("Perf/stage_"):
                stage_seconds[name] = stage_seconds.get(name, 0.0) + float(cast(float, value))
    result = summarize(latencies, wall_seconds=clock() - start, simulated_seconds=physical_seconds, slots=env.num_envs)
    result.update(
        {
            "warmup_steps": warmup_steps,
            "completed_episodes": completed_episodes,
            "stage_mean_s": {name: seconds / steps for name, seconds in stage_seconds.items()},
        }
    )
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", required=True)
    parser.add_argument("--num-envs", type=int, default=1)
    parser.add_argument("--steps", type=int, default=200)
    parser.add_argument("--warmup-steps", type=int, default=20)
    parser.add_argument("--decimation", type=int, help="Use one fixed decimation; omit to retain the Task range.")
    parser.add_argument("--map", dest="map_name")
    parser.add_argument(
        "--output", type=Path, required=True, help="New YAML report path; existing outputs are preserved."
    )
    add_host_flags(parser)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    if args.steps < 1 or args.warmup_steps < 0 or args.num_envs < 1:
        parser.error("steps and num-envs must be positive; warmup-steps must be non-negative")
    if args.decimation is not None and args.decimation < 1:
        parser.error("decimation must be positive")
    if args.output.suffix.lower() not in {".yaml", ".yml"}:
        parser.error("output must be a YAML file")
    output = args.output.resolve()
    run_directory = output.parent / (output.stem + "-run")
    if output.exists() or run_directory.exists():
        parser.error("select new output and Run paths; existing evidence is not overwritten")
    host = resolve_host_flags(args)
    overrides = {"worker.slot_count": str(args.num_envs), "logging.run_directory": str(run_directory)}
    if args.decimation is not None:
        overrides["worker.decimation"] = f"[{args.decimation},{args.decimation}]"
    if args.map_name:
        overrides["session.map_path"] = args.map_name
    base = build_run_config(args.task, overrides=overrides)
    config = build_run_config(
        args.task,
        overrides={
            **overrides,
            **build_launch_overrides(
                ue_executable=host.ue_executable,
                project=host.project,
                map_name=base.session.map_path,
            ),
        },
    )
    report: dict[str, object] = {
        "task": config.task_id,
        "status": "FAIL",
        "training": False,
        "policy_inference": False,
        "events_and_curriculum": False,
        "git_identity": dict(capture_git_identity()),
        "physics_dt": config.worker.physics_dt,
        "decimation": list(config.worker.decimation),
        "run_directory": str(run_directory),
        "python_pid": os.getpid(),
    }
    controller = WorkerProcessController()
    raw = None
    env = None
    exit_code = 1
    try:
        start = perf_counter()
        registry = create_default_registry()
        task = registry.create_task(config.task_id, config.task)
        raw = UERLSession.open(config, process_controller=controller)
        env = UERLDirectEnv(
            UERLSessionAdapter(raw),
            task,
            resolved_config=config,
            profiler=StageProfiler(rollout_length=args.steps),
        )
        report["initialization_seconds"] = perf_counter() - start
        handle = controller.handle
        report["worker_pid"] = handle.process.pid if handle and handle.process else None
        print(f"[BENCHMARK] ready python_pid={os.getpid()} worker_pid={report['worker_pid']}", flush=True)
        report.update(measure(env, steps=args.steps, warmup_steps=args.warmup_steps))
        report["status"] = "PASS"
        exit_code = 0
    except KeyboardInterrupt:
        report["status"] = "INTERRUPTED"
        report["error"] = "KeyboardInterrupt"
        exit_code = 130
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        start = perf_counter()
        try:
            if env is not None:
                env.close("runtime_benchmark_complete")
            elif raw is not None:
                raw.close("runtime_benchmark_setup_failed")
            report["shutdown"] = "PASS"
        except Exception as error:
            report["shutdown"] = f"FAIL: {type(error).__name__}: {error}"
            report["status"] = "FAIL"
            exit_code = 1
        report["shutdown_seconds"] = perf_counter() - start
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("x", encoding="utf-8") as stream:
            yaml.safe_dump(report, stream, allow_unicode=True, sort_keys=False)
    print(f"[BENCHMARK] status={report['status']} report={output}", flush=True)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())

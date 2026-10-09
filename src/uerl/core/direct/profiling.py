"""Per-stage latency instrumentation for one Direct environment step loop.

The profiler measures wall-clock time for the fixed stages of
``UERLDirectEnv.step`` and exposes two aligned outputs:

* Per-step stage seconds returned to the caller, keyed as ``Perf/stage_<name>``
  so the RSL-RL logger averages them across an iteration into TensorBoard.
* An optional structured JSONL record flushed once per iteration (a fixed window of
  ``rollout_length`` steps) for offline analysis.

Both outputs share the same iteration boundary because the RSL-RL runner calls
``env.step`` exactly ``num_steps_per_env`` (``rollout_length``) times per
learning iteration.
"""

from __future__ import annotations

import json
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from time import perf_counter

# Fixed stage names for one ``UERLDirectEnv.step`` transition, in execution order.
STAGE_NAMES: tuple[str, ...] = (
    "preprocess",
    "step_roundtrip",
    "reward_term",
    "terminal_obs",
    "reset_roundtrip",
    "build_obs",
)

# Exclusive client-side components of ``step_roundtrip``. They are persisted
# for accounting and emitted with the existing RSL-RL performance metrics.
DETAIL_STAGE_NAMES: tuple[str, ...] = (
    "client_action_encode",
    "client_request_validate",
    "client_socket_send",
    "client_response_wait_recv",
    "client_response_validate",
    "client_state_decode",
    "client_state_move",
)

_ALL_STAGE_NAMES = STAGE_NAMES + DETAIL_STAGE_NAMES

_METRIC_PREFIX = "Perf/stage_"


class StageProfiler:
    """Accumulate stage latencies and optionally persist one row per iteration."""

    def __init__(self, rollout_length: int, jsonl_path: Path | None = None) -> None:
        """Create a profiler bound to one iteration window and JSONL sink.

        Args:
            rollout_length: Number of ``step`` calls per learning iteration. A
                JSONL row is flushed every ``rollout_length`` steps.
            jsonl_path: Optional destination for structured per-iteration latency
                records. None keeps metrics in memory without file writes.
        """

        if rollout_length < 1:
            raise ValueError("rollout_length must be positive")
        self._rollout_length = rollout_length
        self._jsonl_path = jsonl_path
        self._current: dict[str, float] = {}
        self._window_sum: dict[str, float] = {name: 0.0 for name in _ALL_STAGE_NAMES}
        self._window_steps = 0
        self._iteration = 0

    @contextmanager
    def stage(self, name: str) -> Iterator[None]:
        """Time one stage and record its duration for the current step.

        A stage that is skipped in a given step contributes zero seconds to that
        step so the per-iteration mean reflects real amortized cost.
        """

        start = perf_counter()
        try:
            yield
        finally:
            self._current[name] = self._current.get(name, 0.0) + (perf_counter() - start)

    def commit_step(self) -> dict[str, float]:
        """Close the current step, accumulate the window, and flush per iteration.

        Returns:
            A mapping of ``Perf/stage_<name>`` to this step's stage seconds,
            ready to be merged into ``episode_metrics`` for TensorBoard.
        """

        all_step_seconds = {name: self._current.get(name, 0.0) for name in _ALL_STAGE_NAMES}
        for name, seconds in all_step_seconds.items():
            self._window_sum[name] += seconds
        self._window_steps += 1
        self._current = {}

        if self._window_steps >= self._rollout_length:
            self._flush_window()

        return {f"{_METRIC_PREFIX}{name}": seconds for name, seconds in all_step_seconds.items()}

    def record_step_details(self, timings: dict[str, float]) -> None:
        """Add exclusive client roundtrip components to the current step."""

        for name, seconds in timings.items():
            self._current[name] = self._current.get(name, 0.0) + seconds

    def _flush_window(self) -> None:
        """Write one JSONL record for the completed iteration window and reset it."""

        steps = self._window_steps
        record = {
            "iteration": self._iteration,
            "steps": steps,
            "stage_mean_s": {name: self._window_sum[name] / steps for name in _ALL_STAGE_NAMES},
            "stage_total_s": {name: self._window_sum[name] for name in _ALL_STAGE_NAMES},
        }
        if self._jsonl_path is not None:
            self._jsonl_path.parent.mkdir(parents=True, exist_ok=True)
            with self._jsonl_path.open("a", encoding="utf-8") as sink:
                sink.write(json.dumps(record) + "\n")

        self._iteration += 1
        self._window_steps = 0
        self._window_sum = {name: 0.0 for name in _ALL_STAGE_NAMES}


__all__ = ["DETAIL_STAGE_NAMES", "StageProfiler", "STAGE_NAMES"]

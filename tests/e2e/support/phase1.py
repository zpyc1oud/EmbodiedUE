"""Run bounded UE Worker regressions over the target SocketBridge."""
from __future__ import annotations

import sys
from typing import cast

from tests.e2e.support.host import worker_log_path
from tests.e2e.support.worker_runner import SEED, N, _free_port, _launch, _stop_process, run_session
from tests.protocol.support.socket_client import SocketBridgeClient, SocketBridgeError


def run_negative_gate() -> bool:
    """Launch a bad-physics Worker and verify Initialize rejects it cleanly.

    Returns:
        ``True`` when the Worker reports the expected configuration rejection.
    """

    port = _free_port()
    log_path = worker_log_path("_ue_worker_negative")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = _launch(port, log_file, ["-uerlforcebadgate=1"])
        rejected = False
        try:
            client = SocketBridgeClient("127.0.0.1", port, N, SEED)
            client.connect(timeout_s=120.0)
            try:
                client.initialize()
            except SocketBridgeError as exc:
                rejected = "CONFIG_REJECTED" in str(exc)
                print(f"[VERIFY] invalid physics gate rejected: {exc}")
        finally:
            return_code = _stop_process(proc)
    if return_code == 0 and not rejected:
        raise AssertionError("forced physics gate failure was not rejected")
    return rejected


def run_repeat(runs: int = 3, steps: int = 120) -> bool:
    """Compare bounded same-machine runs against the documented spread tolerance.

    Args:
        runs: Number of independent Worker runs.
        steps: Steps executed by each run.

    Returns:
        ``True`` when every run completes and maximum-cart-position spread is
        within the repeatability tolerance.
    """

    results = [run_session(num_steps=steps) for _ in range(runs)]
    positions = [float(cast(float, result["max_cart_pos"])) for result in results]
    spread = max(positions) - min(positions)
    relative = spread / max(abs(max(positions)), 1e-9)
    print(f"[VERIFY] repeatability relative spread={relative:.2e}")
    return all(result["steps"] == steps for result in results) and relative <= 1e-2


def run_stability(steps: int = 1000) -> bool:
    """Run a bounded Worker trajectory and require progress plus physical motion.

    Args:
        steps: Number of external steps to execute.

    Returns:
        ``True`` when the Worker completes all steps and observes non-zero cart
        motion.
    """

    result = run_session(num_steps=steps)
    return (
        cast(int, result["steps"]) == steps
        and float(cast(float, result["max_cart_pos"])) > 0.01
    )


def main() -> int:
    """Run selected phase regressions and return a process-style status code."""

    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    results: dict[str, bool] = {}
    if mode in ("negative", "all"):
        results["negative_gate"] = run_negative_gate()
    if mode in ("repeat", "all"):
        results["repeatability"] = run_repeat()
    if mode in ("stability", "all"):
        results["stability"] = run_stability()
    for name, passed in results.items():
        print(f"[RESULT] {name}: {'PASS' if passed else 'FAIL'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())

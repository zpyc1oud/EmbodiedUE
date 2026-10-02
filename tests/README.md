# Tests

This directory contains tests, mocks, fixtures, and E2E orchestration. Product interfaces and Tasks belong in `src/uerl`.

## Test layers

| Directory | Scope |
|---|---|
| `python/unit/` | Task mathematics, configuration, and Python components |
| `python/integration/` | Session boundaries and component composition |
| `protocol/unit/` | U4 frames, strict JSON/JCS, and layout golden vectors |
| `protocol/integration/` | Python IPC client with a mock Worker |
| `protocol/support/` | Independent test-only wire oracle and socket client |
| `parity/` | Reviewed operator, action-plan, artifact, and deployment cases |
| `e2e/` | Real UnrealEditor-Cmd processes and lifecycle tests |
| `tooling/` | Installation, packaging, dependency boundaries, and profiling |
| `ue/unit/` | Index of native UE Automation tests |

Native C++ tests must compile inside UBT modules. Their sources live in each module's `Private/Tests/Unit/`; see the [UE test index](ue/unit/README.md).

## Python checks

After installing dependencies:

```powershell
uv run pytest -q
uv run pytest tests/python/unit tests/protocol/unit tests/tooling -q
uv run pytest tests/python/integration tests/protocol/integration -q
```

Default pytest collection excludes real UE E2E. Python coverage includes task math, RobotConfig/RobotSpec, schema/indexing, Session boundaries, manifests, registries, DirectEnv, artifact containers, and export. Protocol tests cover the 48-byte header, strict JSON, safe integers, layout hashes, padding, flags, and TCP fragmentation.

Parity cases compare both implementations against reviewed expected values. Deployment cases compare Python reference execution and UE plan/NNE/action execution with their specified tolerances, including `atol=rtol=2e-5` for deployment inference cases.

## Windows and UE checks

These commands require the UE 5.8 Windows host and a built `UERLHostEditor`. They have not been rerun in the Linux documentation-editing environment. The E2E support runner currently uses the default Epic `UE_5.8` executable path in `tests/e2e/support/worker_runner.py`; custom installations need that test setup reviewed separately from product CLI overrides.

```powershell
# Python, UE Automation, and real UE E2E.
uv run python scripts/run_all_tests.py

# Real U4/U5 Worker tests.
uv run python scripts/run_e2e.py --suite ue

# All real UE suites; missing UE dependencies fail rather than silently skip.
uv run python scripts/run_e2e.py --suite all

# Typed Session / DirectEnv lifecycle.
uv run python scripts/run_e2e.py --suite p2

# P1 Session lifecycle.
uv run python scripts/run_e2e.py --suite p1

# One-iteration CartPole training smoke test; this starts UE and trains.
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --run-dir runs/cartpole_smoke

# Direct orchestration entry points.
uv run python -m tests.e2e.support.worker_runner
uv run python -m tests.e2e.support.phase1 all

# Protocol rejection cases.
uv run python scripts/run_e2e.py --suite ue -k "u4_005 or u4_006"

# Presentation, ownership, failure cleanup, and PIE attach.
uv run python scripts/run_e2e.py --suite ue -k "u5_007 or u5_008 or u5_009 or u5_010"
```

For a custom UE installation, set `$env:UE_ROOT` (or `$env:UE_58_ROOT`) before running the full suite or E2E scripts. These runners use the repository host project.

To invoke UE Automation directly:

```powershell
& '<UE-root>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' `
  engine/UERLHost.uproject /Engine/Maps/Entry `
  '-ExecCmds=Automation RunTests UERL.Unit+UERL.Integration.Worker.SlotCollision+UERL.Integration.Worker.SharedWorldCollision+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003+UERL.Integration.Policy.Contact+UERL.Integration.Policy.Ground+UERL.Integration.Policy.Clock+UERL.Integration.Policy.Controller+UERL.Integration.Policy.Component;Quit' `
  -unattended -nullrhi
```

## Engine behavior covered

Automation/E2E cases exercise transport layouts, seed and safety behavior, binding, physics gates, variable-decimation Step contracts, topology reflection, kinematics, window-end contact, 35-point terrain observations, asset import/reimport, policy plans/NNE, and live component control. Integration cases cover deployment contact/ground queries, cached hits and errors, completed solver clocks and pause, command latching, Start/Stop, and collision isolation.

Lifecycle cases cover request-driven fixed steps, schema/layout negotiation, Ready gates, header-before-payload validation, binary batches, stable errors, timeout cancellation, disconnect handling, sparse reset, headless/viewport trajectory comparisons, process/attached ownership, fatal cleanup, and Editor/PIE attach/reattach. Passing a Python mock test does not validate these engine behaviors.

Active-Step cancellation tests use a post-physics delay available only in `WITH_DEV_AUTOMATION_TESTS` builds. It is test instrumentation, not a product setting or wire configuration.

## Test conventions

1. Test implemented behavior through executable entry points. Do not reserve acceptance/performance gates for hypothetical features.
2. Include the acceptance-case identifier and behavior in test names where the suite uses that convention; organize tests as Arrange / Act / Assert.
3. `[VERIFY]` output records observations and does not replace assertions.
4. Unit tests avoid external processes. Integration tests use test-only mocks; E2E tests launch UE.
5. Mock external dependencies rather than the logic under test. Each case should independently decide pass/fail.

Manager tests use fixed inputs and independent expected constants. DirectEnv boundary tests check the order of termination, reward, curriculum, reset, commands, interval events, and next observation. Sparse reset assertions use stable Slot IDs. Event terms use distinct fixed intervals to expose timer interference. Reset events generate Session reset payloads; interval events run after reset. Only UE Automation/E2E verifies Chaos numerics.

Variable-decimation tests cover 5/35 ms reward integration, discrete failure costs, 20-second termination, randomized initial timeout phases, sparse clock reset, transition discounts and timeout bootstrap, actual-time vibration evaluation, command-direction progress under turns/overspeed/stalling, highest-tier resampling, checkpoint continuation, and fixed-level resume. Old-objective resume is rejected before Worker startup; CartPole retains the fixed-step path.

## Import boundary

Product code is imported from `src/uerl` using the pytest paths in `pyproject.toml`. `tests/protocol/support/` is an independent test-only oracle; product code must not import it. It is not a training entry point.

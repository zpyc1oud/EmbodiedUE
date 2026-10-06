# Tests

This directory contains tests, mocks, fixtures, and E2E orchestration.
Product interfaces and Tasks belong in `src/uerl`.
Use [Write tests](../docs/how-to/write-tests.md) for examples, fixture rules, independent assertions, and review steps.

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

Native C++ tests must compile inside UBT modules.
Their sources are in each module's `Private/Tests/Unit/` directory.
See the [UE test index](ue/unit/README.md).

## Python checks

After dependency installation, run the applicable commands:

```powershell
uv run pytest -q
uv run pytest tests/python/unit tests/protocol/unit tests/tooling -q
uv run pytest tests/python/integration tests/protocol/integration -q
```

Default pytest collection excludes real UE E2E.
Python coverage includes Task mathematics, RobotConfig/RobotSpec, schemas, indexing, Session boundaries, manifests, registries, DirectEnv, artifacts, and export.
Protocol tests cover the 48-byte header, strict JSON, safe integers, layout hashes, padding, flags, and TCP fragmentation.

Parity cases compare both implementations with reviewed expected values.
Deployment cases compare Python reference execution with UE plan, NNE, and action execution.
Use the specified tolerances, including `atol=rtol=2e-5` for deployment inference cases.

Saved-run tests use the application resolver and the play/export CLI through the Session-open boundary.
See `python/integration/test_saved_run_cli_config.py`.
These cases do not start a Worker.
They cover saved settings, map precedence, launch arguments, and snapshot preservation.

Play/export cases cover Task inference, identity conflicts, incomplete snapshots, rejected semantic overrides, and external Task registration.
Continuation cases cover strict metadata, permitted changes, source preservation, and both training configuration builds.

`python/integration/test_resume_checkpoint.py` uses real small CPU RSL models.
It covers normalization, populated optimizer state, iteration, Python curriculum state, and decimation state through the existing checkpoint loader.
These cases do not establish physical behavior or the real UE training procedure.

## Windows and UE checks

These commands require a UE 5.8 Windows host and a built `UERLHostEditor`.
Both runners use the product host profile resolver.
Select `--host-profile`, `UERL_HOST_PROFILE`, or the default `~/.uerl/host.toml`.
Explicit `--ue-executable` and `--project` paths override profile fields.
Relative paths in a profile start at the profile's directory.
If the profile omits the executable, `UE_ROOT`, then `UE_58_ROOT`, supplies the engine root before the default Epic installation.
The default project remains `engine/UERLHost.uproject`.
The runner prints the selected paths and their sources.

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

For a custom host, use the same profile as the product CLI:

```powershell
uv run python scripts/run_e2e.py --suite ue --host-profile E:/uerl/host.toml
uv run python scripts/run_all_tests.py --host-profile E:/uerl/host.toml --timeout 2400 --output-dir E:/uerl/test-results
```

`--timeout` limits each stage in seconds; its default is 1800.
`--output-dir` selects the parent of a new, unique directory.
Its default is `.scratch/test-runs`.
Each invocation writes `summary.yaml`, separate stage output, and separate Automation logs.
Worker helper logs also use unique filenames in that directory.
Direct helper invocations use unique files in the system temporary directory.
The scripts capture detailed output in these files and print stage progress to the console.

The summary records commands, return codes, durations, host paths, and result states.
A stage can be `passed`, `failed`, `blocked`, `timed_out`, `interrupted`, or `not_run`.
A stage in progress is `running`.
The runner stops after an unsuccessful stage and leaves later stages as `not_run`.
Timeout returns 124; operator interruption returns 130.
A missing host returns 2 before the selected UE stage starts.
An Automation process must also produce its current successful completion log.
Existing Automation filters and minimum completion counts remain unchanged.

On timeout or interruption, cleanup targets the stage's own child tree on Windows or its new process group on POSIX.
It does not search for processes by executable name.
A cleanup error remains in the report and requires inspection of the recorded child process.
The test runner changes require a Windows/UE smoke run before a host-validation claim.
Python subprocess tests do not establish Windows process-tree or Unreal behavior.

For direct UE Automation, run the complete core and extended filters.
Use a separate Editor process for each group.
A fresh terrain process prevents order-dependent preview-scene physics.
The command `scripts/run_e2e.py --suite all` includes `PIEAttach` and supplies its port and Python client.

```powershell
$automationGroups = @(
  'UERL.Unit+UERL.Integration.Worker.SlotCollision+UERL.Integration.Worker.VariableDt+UERL.Integration.Worker.SharedWorldCollision+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003+UERL.Integration.Policy.Contact+UERL.Integration.Policy.Ground+UERL.Integration.Policy.Clock+UERL.Integration.Policy.Controller+UERL.Integration.Policy.Component',
  'UERL.Integration.Robot.GenericDrive+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_001+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_002+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_004+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_005+UERL.Integration.Robot.TopologyReflector+UERL.Integration.Worker.EnvironmentPool+UERL.Integration.Worker.Terrain'
)
foreach ($group in $automationGroups) {
  & '<UE-root>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' `
    engine/UERLHost.uproject /Engine/Maps/Entry `
    "-ExecCmds=Automation RunTests $group;Quit" `
    -unattended -nullrhi -nosound -NoSplash
  if ($LASTEXITCODE -ne 0) { throw "UE Automation failed: $group" }
}
```
## Engine behavior covered

Automation and E2E cases cover transport layouts, seeds, safety behavior, binding, physics gates, and variable-decimation Step contracts.
They also cover topology reflection, kinematics, final-step contact, 35-point terrain observations, asset import, reimport, policy plans, NNE, and live control.

Integration cases cover deployment contact and ground queries, cached hits, errors, completed solver clocks, pause, commands, start, stop, and collision isolation.
Lifecycle cases cover fixed steps, schema negotiation, Ready, binary batches, errors, timeouts, disconnects, sparse reset, and trajectory comparisons.
They also cover process ownership, fatal cleanup, and Editor/PIE attachment and reattachment.
Python mock results do not establish these engine behaviors.

Active-Step cancellation cases use a post-physics delay available only in `WITH_DEV_AUTOMATION_TESTS` builds.
The delay is test instrumentation, not a product setting or wire configuration.

## Test conventions

1. Exercise implemented behavior through executable entry points.
   Do not define acceptance or performance requirements for hypothetical features.
2. Use the existing acceptance-case identifier convention and a behavior-based test name.
   Organize the case as Arrange, Act, and Assert.
3. Use assertions to determine the result.
   Use `[VERIFY]` output only for supplementary observations.
4. Keep unit tests free of external processes.
   Use controlled dependencies for integration cases and actual UE for E2E cases.
5. Replace external dependencies instead of the logic under examination.
   Each case must determine its own pass or fail result.

Manager cases use fixed inputs and independent expected constants.
DirectEnv cases cover termination, reward, curriculum, reset, commands, interval events, and the next observation in their required order.
Sparse reset assertions use stable Slot IDs.
Event cases use distinct fixed intervals to expose timer interference.
Reset events create Session reset payloads.
Interval events run after reset.

Only UE Automation or real E2E can establish Chaos numerical behavior.
Variable-decimation cases cover 5/35 ms reward integration, discrete failure costs, and 20-second termination.
They also cover initial timeout phases, sparse clock reset, discounts, timeout bootstrap, vibration evaluation, and command-direction progress.
Progress cases include turns, excessive speed, and no motion.
Other cases cover highest-tier resampling, checkpoint continuation, and fixed-level resume.
Old-objective resume fails before Worker startup, while CartPole retains fixed-step behavior.

## Import boundary

Pytest imports product code from `src/uerl` through the paths in `pyproject.toml`.
The directory `tests/protocol/support/` contains an independent test-only oracle.
Product code must not import it.
It is not a training entry point.

# Testing

[Project home](../README.md)

## Contents

- [Tests](#test-suites)
- [Write tests](#write-tests)
- [UE unit tests](#native-tests)
- [Measure runtime cost without training](#runtime-benchmark)


<a id="test-suites"></a>

<a id="test-suites-tests"></a>
## Tests

This directory contains tests, mocks, fixtures, and E2E orchestration.
Product interfaces and Tasks belong in `src/uerl`.
Use [Write tests](README.md#write-tests) for examples, fixture rules, independent assertions, and review steps.

For several reviewed features, follow [batch validation](../CONTRIBUTING.md#batch-validation).
Run early checks per feature and shared UE checks on the frozen integration candidate.
Record the exact combined revision; a different subset does not inherit its acceptance automatically.

<a id="test-suites-test-layers"></a>
### Test layers

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
See the [UE test index](README.md#native-tests).

<a id="test-suites-python-checks"></a>
### Python checks

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

<a id="test-suites-windows-and-ue-checks"></a>
### Windows and UE checks

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
  'UERL.Unit+UERL.Integration.Worker.SlotCollision+UERL.Integration.Worker.SharedWorldCollision+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003+UERL.Integration.Policy.Contact+UERL.Integration.Policy.Ground+UERL.Integration.Policy.Clock+UERL.Integration.Policy.Controller+UERL.Integration.Policy.Component',
  'UERL.Integration.Robot.GenericDrive+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_001+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_002+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_004+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_005+UERL.Integration.Robot.TopologyReflector+UERL.Integration.Worker.EnvironmentPool+UERL.Integration.Worker.Terrain',
  'UERL.Integration.PhysicsResponse'
)
foreach ($group in $automationGroups) {
  & '<UE-root>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' `
    engine/UERLHost.uproject /Engine/Maps/Entry `
    "-ExecCmds=Automation RunTests $group;Quit" `
    -unattended -nullrhi -nosound -NoSplash
  if ($LASTEXITCODE -ne 0) { throw "UE Automation failed: $group" }
}
```
<a id="test-suites-engine-behavior-covered"></a>
### Engine behavior covered

Automation and E2E cases cover transport layouts, seeds, safety behavior, binding, physics gates, and variable-decimation Step contracts.
They also cover topology reflection, kinematics, final-step contact, 35-point terrain observations, asset import, reimport, policy plans, NNE, and live control.

Integration cases cover deployment contact and ground queries, cached hits, errors, completed solver clocks, pause, commands, start, stop, and collision isolation.
Lifecycle cases cover fixed steps, schema negotiation, Ready, binary batches, errors, timeouts, disconnects, sparse reset, and trajectory comparisons.
They also cover process ownership, fatal cleanup, and Editor/PIE attachment and reattachment.
Python mock results do not establish these engine behaviors.

Active-Step cancellation cases use a post-physics delay available only in `WITH_DEV_AUTOMATION_TESTS` builds.
The delay is test instrumentation, not a product setting or wire configuration.

### Quantitative physics response

Run `UERL.Integration.PhysicsResponse` in a fresh Editor process.
The complete test runner includes this group and requires each named case in `PHYSICS_RESPONSE_CASES` in `scripts/run_all_tests.py`.
An unrelated passing case cannot replace a missing physical test.
These cases use real Chaos simulation. They do not train a policy.
The analytical bodies disable sleep and body inertia conditioning.
Their predictions use the declared geometric inertia, not a solver-adjusted inertia.

Use an independent physical prediction for each case:

| Fixture | Input | Expected response |
|---|---|---|
| Free uniform cube | Constant force at the center of mass | `v = F t / m`; `x = F t² / (2m)` |
| Free uniform cube | Constant torque | `ω = τ t / I`; `θ = τ t² / (2I)`; `I = m L² / 6` |
| Fixed-base hinge with a position drive | Constant external torque below the drive limit | `q = target + τ / Kp`; position and angular velocity settle |
| Fixed-base hinge with a damping-only drive | Initial angular velocity, no external load | `ω(t) = ω(0) exp(-Kd t / I)`; kinetic energy decreases |
| Fixed-base hinge with a saturated drive | External torque above the opposing drive limit | `Δω = (τ_external - τ_limit) t / I`, with the signed counterpart |

The additional native cases cover these boundaries:

- Input lifetime, one-shot impulses, offset forces, rotated local/world frames, asymmetric inertia, and gravity
- Signed free coordinates, locked axes, angular/prismatic hard stops, disabled drives, static angular reaction, and underdamped/overdamped trajectories
- Static support, contact impulse balance, restitution and rebound height, and static/sliding friction
- The real CartPole effort path, declared cart/pole masses, held-force momentum, and reset clearing
- The real CartPole revolute effort path with fixed-axis inertia, signed torque, two masses, rotated roots, and two physics steps
- Two actual provider Slots with distinct forces and impulses, selected reset, and an independent untouched-Slot reference
- Actual PhantomX body inventory, reordered named actuators, completed joint state, whole-body momentum, and weight support
- Authored-to-live mass/constraint checks, articulated COM/link feedback, and query-only geometric support with zero physical force

The PhantomX momentum case checks that initialization does not advance the solver clock.
It checks every body's placement after the first solver step, then applies negative, zero, and positive impulses.

The PhantomX fixed-target case saves bounded CSV traces under `Saved/Automation/PhysicsResponse`.
Each row records the completed solver frame, time, dt, joint name, target, position, and velocity.
Compare unwrapped position change with the trapezoidal velocity integral.
Use an absolute drift budget plus `dt / 2` times total velocity variation.
Check every prefix of the measured interval with its own accumulated budget.
Opposite errors must not cancel at the end of a capture.
Later velocity variation must not relax an earlier failed comparison.
A stationary position with a persistent nonzero velocity must fail.
Keep D1 solver-step consistency separate from D4 control-window comparisons.

`tests/e2e/test_phantomx_physical_response.py` runs real Workers without a learner.
It checks Session wire values against named Python fields and policy-observation slices.
It also compares fresh D1/D4 runs, two-Slot sparse-reset trajectories, and authored/generated flat ground.
The sparse-reset case applies distinct root-push events before a selected reset.
Root-push values are velocity increments; they are not forces in newtons.
It checks cleared velocity/force feedback and the untouched Slot's state trajectory.
The authored fixture uses the registered Walk Task map; an empty Entry map is used only with generated collision ground.
The trace oracle lives in `tests/e2e/support/physical_oracles.py`.
Its unit cases reject missing input, reversed input, wrong units, stale forces, swapped rows, and shifted velocity samples.
These unit cases establish oracle sensitivity, not physical correctness.

Run only these real-process cases with:

```powershell
uv run python scripts/run_e2e.py --suite ue -k physical
```

Keep measured contact impulse, mean support, and the published final-step force separate.
For D4, the final-step force is that step's impulse divided by physics dt.
An interval momentum balance needs the sum of all solver-step impulses.
Do not infer contact torque from a force magnitude or assume equal loading on all six feet.
Use [Issue #67](https://github.com/zpyc1oud/EmbodiedUE/issues/67) for the remaining acceptance scope and reviewed evidence.

The free-body cases vary mass and physics step duration.
They use rotated bodies and a multi-axis input to expose frame errors.
They also stop the input and check momentum, so a stale force cannot pass.
Missing input, reversed input, and a 100-fold input scaling error must fail the same physical oracle.
These are deliberate fixture-input faults; they do not modify production code.

The supported effort path requires positive damping and applies `target - damping * velocity`.
Use that declared law in its physical prediction; zero-gain free-body input is a separate reference fixture.
The CartPole force fixture locks the pole so the independent translation model has total mass `m_cart + m_pole`.
Its velocity follows `v(t) = target/d + (v0 - target/d) exp(-d*t/m)` within each constant-input phase.
The fixed-axis pole uses the corresponding angular solution with the inventoried hinge inertia.
These fixtures preserve the product command path and its documented damping behavior.

The expected response uses declared SI properties and elementary mechanics.
Do not compute it with the production unit-conversion function.
Check actual mass and inertia against the declared fixture before evaluating motion.
For PhantomX, rebuild mass geometry from the authored PhysicsAsset and resolved material.
Compare mass, local COM, and the full body-frame inertia tensor with the live body.
Compare tensor columns rather than principal-axis quaternions, because repeated eigenvalues make those axes ambiguous.
Keep this asset-transfer oracle separate from the elementary-mechanics free-body response oracle.
Read the completed solver clock on each step; a requested time interval is not evidence of an executed interval.

Free-body velocity tolerance is 0.5% of the expected magnitude plus `1e-4` in the applicable SI unit.
Position tolerance is `|a| t dt + 1e-4`, which bounds first-order integration error and shrinks with the step duration.
The hinge equilibrium case allows `0.0025 rad` of position error and `0.005 rad/s` of rest velocity.
The saturation case allows 2% of expected velocity plus `0.001 rad/s`.
The damping-only case uses the continuous exponential solution and an explicit
implicit-Euler endpoint error bound that shrinks with dt, plus `1e-4 rad/s`.
Investigate a failed bound before changing it. Preserve the measured values and the reason for any tolerance change.

These fixtures verify free-body input conversion and the shared Chaos position-drive configuration.
They do not by themselves validate PhantomX mass properties, contact behavior, or a learned policy.
Production assets can retain conditioning settings that change their effective response.
Validate those settings separately with the full-robot cases.
A full-robot case must exercise the Robot/Session path and check its own physical response.
For a feedback-boundary case, match the native capture and wire response by transaction sequence.
The physical E2E fixture records completed solver frame/time, independent joint geometry,
articulated body pose/velocity, and staging values before Transport releases the response.
It runs a rotated robot with reversed field order at D1 and D4.
Its test-only capture switch is compiled under `WITH_DEV_AUTOMATION_TESTS`.
The fixture enables it for a bounded one-Slot run and writes CSV under the test output directory.
Reset captures have an explicit phase and must not advance the solver clock.
Missing capture rows, shifted transactions, mixed frames, and altered values fail acceptance.

Do not label total constraint reaction torque or a PD estimate as measured actuator torque.

The static reaction fixture uses a world anchor offset from the body's COM.
Compare all three force components and all three moment components in the world frame.
In UE 5.8.3, `GetConstraintForce` returns constraint impulses despite its name.
Divide linear output by `100 * physics_dt` and angular output by `10000 * physics_dt` for SI force and moment.
The angular output is a constraint couple. Add the linear reaction's moment arm for a COM balance.
Check the engine source before using this conversion on another engine version.

The contact oracle sums completed manifold-point results.
It uses each point's application position for the external moment balance.
Require a current collision epoch, valid point results, unlimited manifold capacity, disabled CCD,
and disabled split impulse for this momentum-based fixture.
Include both velocity impulse and position impulse divided by physics dt.
Compare their sum with the constraint's accumulated impulse before comparing measured momentum.
A force magnitude and a single nearest contact point cannot establish a distributed contact moment.

For soft limits, declare force mode and physical stiffness and damping explicitly.
The fixture compensates the engine's coefficient scales without changing global settings.
Compare the trajectory with the independent spring-damper solution at two masses and two time steps.
Check steady penetration and release under inward load separately.
The current actuator declaration has no joint-speed limit; do not add one merely to reproduce an upstream test.
A zero velocity drive target is not a speed limit.

Reference patterns come from Isaac Lab revision `b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8`:
[rigid-body force cases](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/test/assets/test_rigid_object.py),
[single-joint static wrench and articulation cases](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/test/assets/test_articulation.py),
and [motor saturation cases](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/test/actuators/test_dc_motor.py).
The Chaos fixtures and predictions are project-specific.

<a id="test-suites-test-conventions"></a>
### Test conventions

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
Command-clock cases check that a new reset command receives its full interval.
The completed window advances continuing Slots, but does not age a new episode command.
Event cases use distinct fixed intervals to expose timer interference.
Reset events create Session reset payloads.
Interval events run after reset.

Only UE Automation or real E2E can establish Chaos numerical behavior.
Variable-decimation cases cover 5/35 ms reward integration, discrete failure costs, and 20-second termination.
They also cover initial timeout phases, sparse clock reset, discounts, timeout bootstrap, vibration evaluation, and command-direction progress.
Progress cases include turns, excessive speed, and no motion.
Other cases cover highest-tier resampling, checkpoint continuation, and fixed-level resume.
Old-objective resume fails before Worker startup, while CartPole retains fixed-step behavior.

<a id="test-suites-test-quality-review"></a>
### Test-quality review

It distinguishes full-file review, focused historical evidence and unreviewed files.
Passing collection or runtime counts do not complete the review.

The CPU examples include distinct full/compact reward rows, clip-before-scale action commands,
a saved policy artifact read through the real export boundary, and mixed terminal/timeout PPO returns.
See [Write tests](README.md#write-tests-example-distinct-slots-and-saved-artifacts).
Temporary faults must fail the intended assertion and be restored; setup errors are not regression evidence.
These CPU cases do not establish C++ parity, Chaos response or learning quality.
The stateful slice adds error-precedence and cleanup-note assertions at the
controlled Session boundary, noncommuting curriculum terms, distinct checkpoint
states and literal sparse event values. Test fixtures restore global RNG/module
state; verification output is limited to the behavior the test actually asserts.


<a id="test-suites-import-boundary"></a>
### Import boundary

Pytest imports product code from `src/uerl` through the paths in `pyproject.toml`.
The directory `tests/protocol/support/` contains an independent test-only oracle.
Product code must not import it.
It is not a training entry point.


The completed Issue #29 review adds real-file rewrite and no-overwrite assertions,
distinct action rows, independent native network/terrain values and sparse E2E reset checks.
Runner tests include unsuccessful/duplicate Automation completions and actual Windows
owned-descendant timeout cleanup. Test-support sockets and packaging commands have bounded waits.

<a id="write-tests"></a>

<a id="write-tests-write-tests"></a>
## Write tests

Use this guide when you add behavior or repair a defect in EmbodiedUE.
The [test guide](README.md#test-suites) gives the suite layout and execution commands.
The [contribution workflow](../CONTRIBUTING.md#contribution-workflow) gives the review and merge requirements.

<a id="write-tests-before-you-start"></a>
### Before you start

1. Identify the behavior that changed.
2. Read the applicable contract in [CONTEXT](../docs/architecture.md#context), the accepted RFC, or the module documentation.
3. Find the nearest existing test.
4. Write down the input, operation, and expected result.
5. Select a test layer from the next section.

The expected result must come from the contract, an independent calculation, or reviewed reference data.
Do not calculate it with the function under examination.
A shape assertion proves a shape requirement.
It does not prove a correct reward, physical response, or reset.

<a id="write-tests-select-the-test-layer"></a>
### Select the test layer

| Layer | Location | Use |
|---|---|---|
| Python unit | `tests/python/unit/` | Mathematics, configuration, masks, and components without an external process |
| Python integration | `tests/python/integration/` | Session contracts and component interaction through a controlled dependency |
| Protocol | `tests/protocol/` | Frame bytes, strict encoding, layouts, and socket behavior |
| Parity | `tests/parity/` | Python and C++ results against reviewed reference values |
| Tooling | `tests/tooling/` | Package installation, discovery, resources, generators, and scripts |
| UE Automation | Module `Private/Tests/Unit/` directories | Native code and supported engine behavior |
| Real UE E2E | `tests/e2e/` | Actual Worker processes, Chaos, reset, presentation, and process ownership |

Choose the smallest layer that can detect the defect.
Add another layer when the change crosses a contract.
For example, a new plan operator needs Python, C++, parity, and applicable engine evidence.
Obey the [operator admission procedure](../docs/architecture.md#operators).

Native C++ files belong in their UBT module.
The directory `tests/ue/unit/` contains an index, not a separate C++ build target.

<a id="write-tests-write-a-python-unit-test"></a>
### Write a Python unit test

Use the existing file when it owns the behavior.
Use a new `test_*.py` file for a separate component.
Name each function after the behavior and its condition.
Keep an existing acceptance-case identifier when that suite uses one.

Separate these parts:

- Arrange: create fixed inputs and the object under examination.
- Act: call the public operation.
- Assert: compare observable results with independent expected values.

<a id="write-tests-example-reward-calculation"></a>
#### Example: reward calculation

Read [test_reward_manager.py](python/unit/test_reward_manager.py).
The function `test_ac_py_unit_rewmgr_001_weighted_sum_with_negative_weight` uses three fixed terms:

| Term | Value | Weight | Contribution |
|---|---:|---:|---:|
| track | 1.0 | 2.0 | 2.0 |
| penalty | 1.0 | -0.5 | -0.5 |
| bonus | 0.25 | 4.0 | 1.0 |

The expected total is 2.5 for each of two rows.
The existing test contains this assertion:

```python
assert torch.allclose(total, torch.full((2,), 2.5))
```

This is an excerpt, not a complete test file.
Use the imports and fixtures in the linked file.
Do not replace the constant with another call to `RewardManager.compute`.

For time-dependent rewards, use both short and long control intervals.
The existing rate-reward case uses 5 ms and 35 ms intervals.
Its expected totals are -2.5 and 0.5.
The discrete fall cost occurs once in each interval.

<a id="write-tests-example-termination-masks"></a>
#### Example: termination masks

Read [test_termination_manager.py](python/unit/test_termination_manager.py).
The first case has three rows with different termination conditions.
It asserts the `terminated`, `truncated`, and reason masks separately.

Use distinct row values.
Identical rows can hide an index error.
For sparse operations, assert the stable Slot IDs and the unchanged Slots.
Do not confuse a compact batch row with its original Slot ID.

<a id="write-tests-example-distinct-slots-and-saved-artifacts"></a>
#### Example: distinct Slots and saved artifacts

The reward case `test_reward_manager_distinct_rows_keep_weights_and_sparse_episode_sums`
uses terms [1, 3, -2] at weight 2 and costs [4, 1, 0] at weight -0.5.
The independent totals are [0, 5.5, -4].
A second compact batch updates stable Slots 0 and 2; per-Slot logs and reset preserve Slot 1.
Identical rows would conceal a first-row broadcast.

[test_policy_export.py](python/unit/test_policy_export.py) exports a real supported Manager declaration.
Its `test_ac_py_unit_export_002_takes_direct_task_objects_not_rebuild` checks returned plan identity,
then reads the saved artifact and executes its restored plan.
A custom member order, scale and clip produce literal two-Slot values.
A local dictionary holding a plan cannot establish that the exporter or file preserves it.

[test_time_aware_ppo.py](python/unit/test_time_aware_ppo.py) combines continuing, terminal
and timeout Slots over 5 ms and 35 ms transitions.
The worked returns distinguish true-terminal cutoff from current-duration timeout bootstrap.
Its absolute tolerance of 1e-5 covers rounded independent constants and float32 recurrence.

<a id="write-tests-control-fixtures-and-state"></a>
### Control fixtures and state

A fixture supplies test data, objects, or resources with a defined lifetime.

Use `tmp_path` for temporary files.
Use a separate output directory for each invocation.
Do not write test output into checked-in assets or an existing Run.

Use `monkeypatch` for temporary environment variables and dependency replacement.
Restore process-wide state after the case.
Prefer function-scoped mutable fixtures.
Use broader fixture scope only when the resource supports safe reuse.

Set the relevant seed explicitly when random behavior affects the assertion.
Record the seed for a failed randomized case.
Do not assume that one seed makes two different engines numerically identical.

For reset behavior, distinguish three states:

1. The input state used for the action.
2. The transition state before reset.
3. The state returned after reset.

Assert terminal values before reset replaces them.
Assert that reset affects only the selected Slots.
Include repeated reset and failure cleanup when the changed contract permits those operations.

<a id="write-tests-write-an-integration-test"></a>
### Write an integration test

Read [test_session.py](python/integration/test_session.py).
Its `_FakeBridge` supplies a controlled Session dependency through `bridge_factory`.
The real Session code still performs the operation.

Replace the external dependency, not the logic under examination.
For example, a Session test can record Bridge requests and supply known responses.
It must not replace the Session method whose behavior it claims to prove.

Assert call order only when the order is part of the public contract.
Manifest creation before Ready is such a contract.
An internal helper call is not sufficient evidence of correct behavior.

Include the relevant failure path:

- A dependency fails before initialization.
- Initialization stops after it acquires a resource.
- A request times out.
- A connection closes during an operation.
- Cleanup occurs after an exception.

Assert the resulting state and the owned resource release.
Do not only assert that an exception occurred.

`test_session_preserves_primary_failure_when_cleanup_also_fails` exercises the
real Session through controlled Bridge and process-controller boundaries.
It asserts the original protocol exception, both cleanup failures as notes,
released ownership, and the resulting lifecycle state.
The fake boundary does not establish OS process termination or Chaos behavior.
Use distinct named checkpoint values and noncommuting transforms to detect
term mixups and execution-order defects, as in the curriculum-manager tests.


<a id="write-tests-write-protocol-and-parity-cases"></a>
### Write protocol and parity cases

Read [test_bridge_codec.py](protocol/unit/test_bridge_codec.py).
Its cross-language header case uses literal expected bytes.
Keep the reference independent of the encoder under examination.

Include malformed data that the supported interface can receive.
Examples include duplicate keys, non-finite values, unsafe integers, and fragmented input.
Do not add unrelated defensive cases without a reachable project requirement.

The [socket support tests](protocol/integration/test_socket_bridge_client.py) exercise the test-support client.
They do not, by themselves, establish correct product-client behavior.

For an operator, add reviewed data under `tests/parity/cases/<operator>/`.
Use [test_operator_parity.py](parity/test_operator_parity.py) as the Python entry point.
Both implementations must match the reviewed expected result.
Agreement between two implementations can preserve the same defect.

Keep protocol and parity fixture formats unchanged.
The use of YAML for project configuration does not change these JSON contracts.

State the reason for each numerical tolerance.
Use exact equality for discrete values where the contract requires it.
Do not increase a tolerance to hide a failed assertion.
Deployment inference fixtures use their specified tolerances, including `atol=rtol=2e-5`.
Do not apply that limit to an entire physical rollout without a separate basis.

<a id="write-tests-write-package-and-generator-tests"></a>
### Write package and generator tests

Read [test_external_task_package.py](tooling/test_external_task_package.py).
Build the actual package.
Install it into an isolated target.
Run the probe outside the source directory.

Make sure that discovery does not depend on the source checkout.
Make sure that resources remain accessible after removal of staged source files.
Make sure that Task construction and CLI configuration meet the contract.
Assert that package import does not change built-in defaults.

For generator changes, use [test_cli_new.py](python/unit/test_cli_new.py).
Include the generated result in the relevant package test.
A source-text assertion cannot replace an installation test.

<a id="write-tests-write-ue-automation-cases"></a>
### Write UE Automation cases

Read [UERLActuatorLawTests.cpp](../engine/Plugins/UERLEngine/Source/UERLRobot/Private/Tests/Unit/UERLActuatorLawTests.cpp).
It uses `IMPLEMENT_SIMPLE_AUTOMATION_TEST` inside `WITH_DEV_AUTOMATION_TESTS`.
Its position-PD case has an independent expected effort of 6.5.

Use the module's existing test flags and name prefix.
Build the Editor target after C++ changes.
Run the exact changed case before its applicable group.

For a physical requirement, use the actual engine object and completed solver state.
A pure arithmetic case does not establish correct Chaos integration.
For lifecycle changes, include start, stop, repeated use, reset, and applicable callback interruption.

Keep the assertion output and completed case names.
A success count alone cannot identify which cases ran.
`[VERIFY]` messages supplement assertions.
They do not replace them.

<a id="write-tests-write-real-ue-e2e-cases"></a>
### Write real UE E2E cases

Start with [test_cartpole_worker.py](e2e/test_cartpole_worker.py)
and [test_p2_direct_env.py](e2e/test_p2_direct_env.py).
Use the existing process and Session helpers.

Give the case a bounded timeout.
Keep enough output to identify the failing stage.
In cleanup, release only processes and files owned by that invocation.
Do not terminate an unrelated Editor or all processes with the same executable name.

Assert the changed physical or lifecycle effect.
Where isolation matters, also assert an unaffected Slot.
For a timeout, distinguish failure before Worker startup from a failed physical assertion.

Use actual authorized assets for a content-dependent case.
A Git LFS pointer is not a usable asset.
Record missing content as a blocker.
Do not substitute another scene and report the original requirement as passed.

<a id="write-tests-run-the-checks"></a>
### Run the checks

Run commands from the repository root.

1. Install the locked development dependencies.

   ```powershell
   uv sync --locked --group dev
   ```

2. Run the affected case.

   ```powershell
   uv run pytest tests/python/unit/test_reward_manager.py::test_ac_py_unit_rewmgr_001_weighted_sum_with_negative_weight -q
   ```

   The selected case must complete and pass.
   A collection result with zero cases is not a pass.

3. Run the affected file.

   ```powershell
   uv run pytest tests/python/unit/test_reward_manager.py -q
   ```

4. Run the checks required by the change.

   ```powershell
   uv run pytest -q
   uv run ruff check src scripts tests
   uv run mypy src tests
   ```

Default pytest collection excludes real UE E2E.
GitHub CI currently runs Ruff and Mypy only.
A green static check is not runtime evidence.

On a built Windows/UE host, select the applicable engine scope:

```powershell
uv run python scripts/run_e2e.py --suite ue
uv run python scripts/run_e2e.py --suite p2
uv run python scripts/run_e2e.py --suite all
uv run python scripts/run_all_tests.py
```

Use only the commands needed for the selected scope.
The full runner already includes Python, UE Automation, and E2E.
Do not repeat an unchanged successful suite without a specific reason.

For custom UE paths, set `UE_ROOT` or `UE_58_ROOT`.
The E2E Worker runner selects `UE_ROOT` first.
It then selects `UE_58_ROOT`, or the default Epic installation.
See the [test guide](README.md#test-suites) for native filters and commands.

<a id="write-tests-prove-a-regression-repair"></a>
### Prove a regression repair

1. Add a case that reproduces the actual defect.
2. Run it against the defective implementation, where practical.
3. Make sure that the intended assertion fails.
4. Apply the repair.
5. Run the case again.
6. Run the affected aggregate checks.

An import error is not proof that a behavioral regression test works.
If the earlier revision cannot run, record that limitation.
Do not weaken the assertion to obtain a pass.

<a id="write-tests-review-the-evidence"></a>
### Review the evidence

Before review, answer these questions:

- Does each assertion establish the changed contract?
- Are the expected values independent?
- Are fixtures and temporary resources isolated?
- Are failure paths and cleanup covered where relevant?
- Are reset values and Slot IDs distinct?
- Does the evidence require actual UE execution?
- Do the selected and completed case names agree?
- Are the tolerances justified?
- Do the commands use the tested revision?
- Are unexecuted checks identified?

Keep the commit, any dirty diff, command, tool versions, seed, and report location.
Use the workflow result states: Passed, Failed, Skipped, or Blocked.
A required skipped or blocked case prevents a full-pass claim.

Keep startup smoke, numerical parity, physical response, and learning quality separate.
One training iteration establishes that a path can execute.
It does not establish policy convergence or reliable scene behavior.

<a id="write-tests-test-documentation-checks"></a>
### Test documentation checks

Documentation tooling cases belong in `tests/tooling/test_documentation_checks.py`.
Use temporary Markdown files with one intended defect.
For a missing link or heading, assert the source line and destination in the diagnostic.
Include a valid link to show that the check distinguishes the two cases.
For CLI examples, test an unknown flag, a missing required argument, and a valid command.
Use the product parser without calling the command implementation.
A documentation check must not launch training, UE, or a shell command from an example.

Run the repository-wide check before you submit documentation changes:

```powershell
uv run python scripts/check_docs.py
```

See [the documentation check](../CONTRIBUTING.md#contributing-check-documentation) for supported syntax and manual review requirements.



<a id="native-tests"></a>

<a id="native-tests-ue-unit-tests"></a>
## UE unit tests

Native UE Automation tests compile in their UBT modules.
The C++ source directories are:

- `engine/Plugins/UERLEngine/Source/UERLInterface/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTransport/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLTerrain/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLWorker/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLRobot/Private/Tests/Unit/`
- `engine/Plugins/UERLEngine/Source/UERLPolicy/Private/Tests/Unit/`

`UERLRobot` cases cover topology reflection, observation plans, kinematics, contact, and provider registration.
They also cover rejected configurations and real CartPole content smoke tests.
The content smoke requires the repository Skeletal Mesh and PhysicsAsset.
Report missing assets as a failure or blocker, not a silent skip.

This directory is an index.
Keep each C++ source in its compilable module.
Use the [test guide](README.md#test-suites) for commands.
Use [Write tests](README.md#write-tests) for test implementation and review.

<a id="runtime-benchmark"></a>

<a id="runtime-benchmark-measure-runtime-cost-without-training"></a>
## Measure runtime cost without training

Use this benchmark to inspect the existing Worker and DirectEnv step path.
It sends zero policy actions. It does not create a policy network, run PPO, tune
parameters, or measure learned behavior. Curriculum adaptation and training
events are not installed.

<a id="runtime-benchmark-run-a-bounded-case"></a>
### Run a bounded case

Build the matching UE host first. Select the same source revision for Python and
UE. Use a new output path for each case:

```powershell
uv run python scripts/benchmark_runtime.py --task UERL-PhantomX-Walk-v0 --num-envs 1 --decimation 4 --warmup-steps 20 --steps 200 --output artifacts/runtime/phantomx-1.yaml
```

The usual `--host-profile`, `--ue-executable` and `--project` options select the
host. `--map` overrides the Task map. Omit `--decimation` to retain the Task's
configured interval range. The report records the effective choice.

The script creates a sibling Run directory for Session evidence. It refuses an
existing report or Run path. It writes a YAML result on success or runtime
failure and closes only the Session that it owns. Invalid or faulted Slots
abort the case. Ordinary Task episode termination is counted separately.

The profiler returns stage metrics in memory. The benchmark does not create a
new client stage JSONL stream. Existing Worker/Session logs retain their normal
formats in the Run directory.

<a id="runtime-benchmark-read-the-report"></a>
### Read the report

- `initialization_seconds` covers Task construction and Session initialization.
- `warmup_steps` are excluded from measured latency and physical time.
- `wall_seconds` covers the measured loop, including its validity/accounting work.
- `step_latency_s` gives mean, median and linearly interpolated p95 latency for
  each step and its output-validity checks.
- `control_steps_per_wall_second` counts synchronized control windows.
- `slot_transitions_per_wall_second` multiplies control windows by the Slot count.
- `physical_seconds_per_slot` sums actual completed intervals.
- `physical_seconds_per_wall_second` is the per-Slot simulation-time ratio.
- `shutdown` and `shutdown_seconds` report Session cleanup separately.

Client detail stages are inside `step_roundtrip`. Do not add them to the parent
stage when calculating total time. These are host wall-clock measurements,
not GPU kernel timings. Do not compare throughput figures with different
Slot counts, timing ranges, scenes or measured horizons without recording those
differences.

<a id="runtime-benchmark-check-memory-and-repeatability"></a>
### Check memory and repeatability

Start with one Slot. If resources permit, run separate 16-Slot and 64-Slot cases
with the same task, decimation, warmup and measured steps. Repeat each selected
case three times with new output names. Compare spread as well as the mean.

On Windows, record system committed bytes and commit limit before, during and
after each case. Record private bytes for the exact Python and UE PIDs printed
by the script. Record GPU memory separately if it is relevant. Available RAM
alone does not establish commit capacity. System commit includes other programs;
do not attribute all of it to this benchmark.

Do not run an additional model-loading analysis process during a memory-limited
case. Do not start concurrent UE instances, change pagefile settings, or close
unrelated programs to improve the reported result. Stop the owned case if
allocation fails or commit usage continues toward the limit. Preserve the
partial report and logs.

The first supported parallel count is the largest measured count with sufficient
host headroom and stable repeated timing. It is not a universal product limit.
A small smoke training/export check can validate its separate path later; a long
learning run is not required to collect this runtime baseline.

<a id="runtime-benchmark-compare-an-optimization"></a>
### Compare an optimization

Pin the baseline and candidate commits. Rebuild UE when native source changes.
Use the same host, Task, map, seed, Slot counts, decimation and measurement horizon.
Keep the raw reports from both revisions. Run the short cases three times per
revision, then run at least 2200 measured steps at D4 for the 20-second PhantomX
Task. Record the completed episode count and Worker resets.

Compare control-step latency, Slot throughput, stage cost and process memory.
Report the individual runs and their spread. A mean of per-run p95 values is not
a pooled p95. Separate Python private bytes from UE private bytes and system
commit. Configuration-only import savings do not establish lower learner memory.

Before accepting a speedup, run the affected native and real UE checks. Replay
the same bounded action sequence at both revisions. Compare physical state,
observations, fault handling and reset behavior with the existing tolerances.
Do not change solver settings or reduce sensing to improve this comparison.

### Training debug checks

The debug recorder must not change training results.
Use real small CPU PPO models to compare model parameters, optimizer state, and RNG state
with tracing enabled and disabled. Include capture expiry while training continues.
Test both fixed-time PPO and TimeAwarePPO.

Exercise multiple Slots with true termination, timeout, and sparse reset.
Keep decision state, completed transition state, and post-reset state distinct.
Reward capture must use the actual computation once; do not recompute stateful reward terms.
Use independent NumPy calculations for normalized inputs, Gaussian likelihoods, action targets,
timeout bootstrap, and GAE. Inject a Slot swap or an incorrect target and require a failing audit.
A passing CPU test does not establish UE contact or actuator behavior.

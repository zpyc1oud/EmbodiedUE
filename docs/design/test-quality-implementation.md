# Repository-wide test quality design

Status: Proposed. Implementation tracker: [Issue #29](https://github.com/zpyc1oud/EmbodiedUE/issues/29).

## 1. Purpose and baseline

Make failures in supported behavior visible at the smallest useful test layer.
Keep effective tests. Change a test only when its review identifies a missing contract, a weak oracle, or avoidable cost.
This document specifies the implementation. It does not report completion of the test-quality audit.

Source baseline: `352ec4a827e1801e26a2bf891a9b174b9cb58200`, inspected on 2026-10-07.
Follow the [contribution workflow](../contribution-workflow.md),
[batch validation](../how-to/batch-validation.md), and [test-authoring guide](../how-to/write-tests.md).
The [earlier audit](../testing-audit-2026-10.md) supplies history. Refresh findings against this baseline before editing tests.

### Existing work

| Work | Current contribution | Treatment in this design |
|---|---|---|
| Issue #19 | Completed audit and Windows follow-up | Reuse evidence at its recorded revision; perform the remaining assertion review under #29 |
| PR #25 | Test-authoring guide and documentation | Extend concrete examples after implementation |
| PR #26, integrated through #30 | Host resolution, timeouts, owned-process cleanup, YAML reports | Keep runner infrastructure; test behavior at its callers |
| PR #27 | Documentation link and CLI-example checks | Use the checker; retain negative checker fixtures |
| PR #28 | Short-lived integration batches | Share applicable UE validation on a frozen candidate |
| [PR #33](https://github.com/zpyc1oud/EmbodiedUE/pull/33) | Selected host propagation into E2E Sessions and CLI children | Reuse this focused repair. Do not duplicate it in the design PR |

PR #33 is Draft at `1bd225d9e162087f301cf1b48d6194ce00338e8e`.
Its recorded focused checks pass. Its final full Python and real E2E checks remain pending.
An earlier full Python run failed in an installed-wheel child with Windows commit-memory exhaustion.
This is an execution failure, not proof that the host-propagation assertions failed.
No existing PR inspected for this proposal covers all workstreams in #29.

## 2. Reference design decisions

Use upstream designs to choose assertions, not to import a second simulator into the test suite.

| Reference | Pattern to use | EmbodiedUE application |
|---|---|---|
| [Isaac Lab termination tests](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/test/managers/test_termination_manager.py) | State transitions, persistent episode information, separate timeout and termination masks | Check state before step, after step, and after a sparse reset |
| [Isaac Lab task smoke](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab_tasks/test/test_environments.py) | Registered-task parametrization | Run supported Task/capability combinations, with explicit exclusions |
| [Isaac Lab ray-caster tests](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/test/sensors/test_ray_caster.py) | Controlled geometry and known ray results | Verify hit geometry, ordering, frames, and loss of support in small UE scenes |
| [MuJoCo sensor tests](https://github.com/google-deepmind/mujoco/blob/92b8f4b270a43d0b31baed458e7692b046ca9e3f/test/engine/engine_sensor_test.cc) | Small models with hand-picked frame values and sensor-time assertions | Derive expected units, rotations, and completed-step values independently |
| [MuJoCo derivative tests](https://github.com/google-deepmind/mujoco/blob/92b8f4b270a43d0b31baed458e7692b046ca9e3f/test/engine/engine_derivative_test.cc) | Independent numerical checks with explicit error bounds | Use finite differences only for smooth functions that need them |
| [MuJoCo step benchmarks](https://github.com/google-deepmind/mujoco/blob/92b8f4b270a43d0b31baed458e7692b046ca9e3f/test/benchmark/step_benchmark_test.cc) | Separate timing measurement from correctness | Record expensive stages without hardware-dependent correctness thresholds |

Isaac Lab's termination test starts its simulator. EmbodiedUE's pure Python manager tests can remain CPU-only.
MuJoCo trajectories and tolerances are not exact reference results for Chaos.

## 3. Inventory and review record

Start implementation with a complete tracked-file inventory at the chosen commit.
Include Python tests, example-package tests, native tests, helpers, fixture generators, fixture corpora, and collection and runner configuration.
Include manual check scripts, but label them separately from executable tests.

Store the review in a small YAML file under `docs/testing/`.
Use one record per test or support file. Corpus records may describe a directory only if they list every member path.
The record contains:

- Path, layer, contract, and required host or dependency
- Oracle: literal, independent formula, reviewed fixture, physical property, or boundary interaction
- Finding status: reviewed-adequate, confirmed-gap, review-candidate, or not-reviewed
- Disposition: keep, strengthen, add-cases, move, consolidate, or remove
- Reachable failure, planned assertion, owning implementation PR, and evidence location

These are proposed review fields, not a new runtime report schema.
Compare recorded paths with tracked paths at both the initial and final implementation commits.
Resolve additions and removals before closure. Do not convert a directory scan into a claim that every assertion was reviewed.
Function counts, collected parameter cases, and executed cases are different measurements.

### Findings already established by this source review

1. Keep the independent reward arithmetic in `test_reward_manager.py`, including the rate-reward example with totals -2.5 and 0.5.
2. Keep the scale-before-clip and explicit entity-order results in `test_observation_manager.py`.
3. Strengthen the export-binding claim in `test_ac_py_unit_obsmgr_003_plan_object_identity`.
   It inserts `manager.plan` into a local dictionary and checks the same reference.
   That check cannot detect the exporter choosing another plan.
   Move the export claim to the real artifact/export boundary; retain an identity check only if it protects a documented identity contract.
4. Keep the hand-worked return and timeout-bootstrap values in `test_time_aware_ppo.py`.
   Review additional mixed termination and duration sequences before deciding that more cases are needed.
5. Keep Session tests that use `_FakeBridge` through `bridge_factory` while executing the real Session.
6. Reuse the confirmed host-propagation repair in #33 and its pre-fix failures.

All other matrix entries below are review targets. They are not assertions that current tests are defective.

## 4. Python unit tests

| Target files in `tests/python/unit/` | Scenario and independent oracle | Failure to detect |
|---|---|---|
| `test_reward_manager.py` | Keep existing arithmetic. Add distinct per-Slot terms where identical rows can hide broadcast errors. For terms `[1, 3, -2]` with weight 2 and costs `[4, 1, 0]` with weight -0.5, expect `[0, 5.5, -4]` | Wrong row broadcast, weight sign, or term order |
| `test_action_manager.py`, `test_action_plan.py`, `test_robot_action.py` | Use asymmetric actions and joint scales. Assert clip, scale, offset, and physical targets in their specified order | A correct shape with wrong command values |
| `test_observation_manager.py`, `test_plan_executor.py`, `test_robot_observation.py` | Two joints and at least two Slots with distinct values; assert exact field order and independent frame transforms | Swapped fields, wrong units, or a shared row |
| `test_termination_manager.py`, `test_direct_env.py` | Three Slots: continuing, terminated, and truncated. Check masks, reasons, terminal observations, returned reset values, and the unchanged continuing Slot | Termination/timeout conflation or terminal data overwritten by reset |
| `test_event_manager.py`, `test_curriculum_manager.py` | Sparse reset of Slots 0 and 2 with distinct Slot 1 state; assert state and counters before and after | Compact row index used as stable Slot ID |
| `test_time_aware_ppo.py`, `test_rsl_rl_wrapper.py` | Short and long durations followed by terminal and timeout transitions; manually derive bootstrap and recurrence values | Previous duration used for current discount or bootstrap after true termination |
| Configuration, continuation, and checkpoint unit files | Valid YAML, duplicate-key rejection, explicit path errors, and strict saved-configuration contracts | Silent fallback, wrong override precedence, or source mutation |

Implement each scenario in its existing owning file unless it tests a separate component.
Use readable parameter IDs such as `sparse-reset-middle-slot-unchanged`.
Use exact equality for masks, IDs, and field order. Derive floating tolerances from the operation and dtype.
Do not duplicate a good case merely to rename it.

### Reset sequence example

Arrange three distinct observations and nonzero previous actions.
Execute one step that terminates Slot 0 and truncates Slot 2.
Assert both terminal observations before reset replacement.
Assert that Slot 1 retains its episode counter and action history.
Execute the next step. Assert that only Slots 0 and 2 start from the specified reset state.
The test fails if reset clears the whole batch or if terminal observations become reset observations.

## 5. Python integration tests

Use `tests/python/integration/test_session.py`, `test_bridge_session.py`,
`test_direct_task_entrypoint_equivalence.py`, `test_resume_checkpoint.py`,
`test_train_resume_config.py`, and `test_saved_run_cli_config.py` as owners.

| Seam | Setup and operation | Required assertions |
|---|---|---|
| Session to Bridge | Controlled Bridge response; real Session initialization and step | Manifest before Ready; correct description/commit data; expected state after success and failure |
| Direct and Manager entry points | Same small deterministic task; fixed nonzero actions | Each result matches a separate literal or formula oracle; agreement alone is insufficient |
| DirectEnv transition | Controlled external step with a faulted middle Slot | Valid-Slot filtering, stable IDs, action mapping, terminal values, and unaffected state |
| Strict continuation | Small real CPU model; take an optimizer step before saving | Model tensors, nonempty optimizer state, normalization, iteration, RNG and supported curriculum/decimation state restored |
| CLI to Session | Real parser and configuration resolution; replace only process creation | Saved identity, explicit allowed overrides, selected host/map, and unchanged source snapshot |
| Partial setup and close | Fail before and after acquiring the external resource | Owned resource released once as required; repeat close safe; primary error remains visible |

For continuation, compare the next deterministic CPU update when the supported saved state is sufficient.
Do not claim physical-state continuation from a Python checkpoint test.
Failures from an actual package import remain failures; they must not become broad exception-to-skip conversions.

## 6. Protocol tests

Keep literal headers and independent support in `tests/protocol/`.
Review the product client separately from `tests/protocol/support/socket_client.py`.
A support-client pass establishes only the support client's behavior.

| Case | Input or schedule | Public-boundary result |
|---|---|---|
| Fragmented frame | Split a fixed frame in header and payload regions | One correct decoded message after all bytes arrive |
| Premature disconnect | End input before the declared frame length | Specific protocol/transport failure; no partial success |
| Invalid identity | Valid framing with wrong sequence, Session, or layout | Rejection in the documented phase and documented connection state |
| Partial send | Controlled socket returns a short write, then completes or fails | Complete write or observable failure; no lost suffix |
| Ambiguous Step timeout | Timeout after a state-changing request may have reached Worker | No automatic duplicate Step; subsequent usability follows the current contract |
| Strict payload | Supported malformed lengths, non-finite values, or duplicate keys | Deterministic rejection without a product state transition |

Use literal expected bytes, not encoder output, to construct the golden oracle.
Keep wire fixtures in their actual contract format. Use YAML for new human-maintained audit metadata.
Do not redesign the wire format under a test-quality repair.

## 7. Cross-language and deployment parity

Owners: `tests/parity/` and the corresponding `UERLPolicy` and `UERLRobot` native tests.
Build a correspondence table from each required corpus category to its Python test and UE Automation name.
For each changed category, record whether both sides actually ran on the final candidate.

Test four values separately: raw observation, processed model input, policy action, and physical target.
Use a multistep fixture with nonzero previous actions, a reset boundary, and two supported control intervals.
Assert the expected value at each step rather than only the last value.
Negative fixtures must state the expected failure phase.

Retain existing operator, action-plan, artifact, deployment, and variable-dt corpora.
Golden changes need an independent derivation or reviewed provenance.
A generator must not silently accept the current implementation as truth.
Report case, step, Slot or field, actual value, expected value, and tolerance on failure.
Retain established inference tolerances where they apply; physical rollouts require their own error budget.

Repair the observation export-binding gap through the real public export path.
Build a Task with distinguishable observation terms, export its artifact, and inspect or execute the serialized plan.
Assert the expected order and values. Changing the exporter to select a default plan must fail this test.
Use the existing artifact/parity tests instead of adding a test-only export wrapper.

## 8. Tooling and public workflows

Owners: `tests/tooling/`, CLI tests, generator tests, and E2E support.

1. Retain actual sdist/wheel construction and isolated installation.
   Run the probe outside the checkout after staged sources are removed.
   Assert installed import paths, packaged YAML, discovery, duplicate registration, and public Task construction.
2. For generated Tasks, execute registration and capability preflight through installed entry points.
   Keep template assertions for syntax details, but do not treat them as installation evidence.
3. Use #33 for host selection. Its regression must cover all direct Session constructors and train/export children.
   Test distinct temporary executable and project paths so the repository default cannot pass accidentally.
4. Preserve runner timeout and owned-process tests from #26/#30.
   Assert result state, return code, cleanup effect, and preserved unrelated process.
5. Keep documentation checker tests isolated in temporary files.
   Check one invalid destination or flag with a useful diagnostic, plus a valid control case.

Run resource-heavy installed-wheel and real UE stages serially on the Windows validation host when memory is constrained.
Record a resource failure as failed or blocked with its cause. Retain the original failed log.
A retry after a verified resource repair is a separate invocation.

## 9. UE Automation and physical fixtures

Keep native cases in their owning UBT modules under `Private/Tests/Unit/`.
The directory `tests/ue/unit/` remains an index.

| Module and existing owners | Controlled setup | Independent behavior to assert |
|---|---|---|
| UERLInterface: types and plan tests | Small explicit layouts and plans | Units, field order, valid dimensions, and rejected supported invalid plans |
| UERLTransport: transport tests | Fixed frame bytes and bounded connection schedule | Framing, phase errors, disconnection, and cleanup |
| UERLTerrain: atlas and generator tests | Plane plus an asymmetric step; known sample positions | Height values, yaw/order, excluded objects, misses, and cache reset |
| UERLWorker: environment pool, Slot collision, terrain ownership | Two distinguishable Slots and one sparse reset | Ownership, isolation, selected environment, unaffected Slot, and teardown |
| UERLRobot: kinematics, contact, actuator, physics clock | Known pose, one controlled action, completed solver callback | Metres/radians, frame transform, geometric support 0/1, effort response, and elapsed solver time |
| UERLPolicy: component, physics gate, variable-dt and deploy parity | Known artifact, initial reset input, then two control windows | First-decision phase, previous-action history, valid timing, reset, and claimed-body restoration |

For a downward ray over a plane, derive distance from the known start and plane height in a common frame.
Move the root and rotate yaw to detect world/root frame confusion and sample-order errors.
Move the ray off the plane to detect a stale cached hit.
Check that a second Slot's geometry cannot provide the first Slot's hit where isolation is required.
These are candidate cases for current terrain contracts; they do not assume a future generic sensor API exists.

For actuator arithmetic, retain the existing independently calculated PD effort example.
For Chaos response, assert a justified direction or bounded physical result in a controlled scene.
Do not impose exact cross-engine trajectories or energy conservation in a damped contact scene.
Read physics-dependent observations after the completed solver step, not an arbitrary game Tick.

Prefer generated small scenes or existing minimal assets.
Keep authorized-content and target-map acceptance separate where the feature requires them.
Camera-policy tests enter this suite only when the input contract and implementation exist.

## 10. Real UE E2E

Use existing Session and Worker helpers in `tests/e2e/`.
Define a Task/capability matrix from the current registry, not a copied list that silently omits new registrations.
Review that list before invocation. Each exclusion needs a capability or content reason.

For each supported smoke entry, test one Slot and a small meaningful batch, normally two Slots.
Use fixed actions or recorded seeds, reset, bounded step, and close.
Check output validity as smoke. Use a separate case for numerical or physical expectations.

Representative acceptance flows are train, strict resume, finite play, supported export, and deployment.
Select them by changed behavior, rather than run every workflow for every test edit.
External Task acceptance must use installed artifacts when package independence is the claim.
Test timeout and partial-start cleanup through an owned process tree.
Do not infer shutdown from an exit code alone when a child process can survive.

Capture selected and completed identities where the runner exposes them.
A missing required case, zero-case selection, duplicate completion, or failed cleanup must prevent a full-pass claim.
Add a narrowly scoped parser repair only after a concrete reporting defect is established.

## 11. Bounded regression demonstrations

Use a small set of representative defects, not a new global mutation framework.

| Demonstration | Expected detection |
|---|---|
| Reverse clip/scale in a temporary candidate | Unit arithmetic assertion fails on a discriminating input |
| Reset all Slots instead of the sparse mask | Continuing Slot state assertion fails |
| Select a default observation plan during export | Artifact-boundary test fails on order or values |
| Drop selected host in an E2E child | #33 regression fails at Session or subprocess boundary |
| Swap a protocol header field or shorten a declared frame | Independent wire oracle rejects or mismatches bytes |
| Retain previous action after policy reset | Sequential parity/native case fails after reset |

Prefer a real pre-fix revision for an established defect.
Otherwise inject a bounded temporary fault in an isolated checkout, observe the intended assertion, and restore the source.
Do not commit a mutation or weaken an assertion. An import or setup error is not successful defect detection.
Record the exact failing test and the restored passing result.

## 12. Implementation order and acceptance

Use focused PRs under #29, without child issues.

| Slice | Deliverable | Gate |
|---|---|---|
| A: inventory | Full tracked-file review record and confirmed findings | Every path accounted for; proposed edits identify a reachable failure |
| B: CPU assertions | Unit/integration/export-binding improvements and regression demonstrations | Focused tests, default Python suite, Ruff and Mypy on the final candidate |
| C: boundary tests | Protocol/parity/tooling gaps; integrate #33 after its evidence is complete | Independent fixtures, package evidence, and selected-host regression; native parity pending explicitly |
| D: native behavior | Small UE fixtures and native parity gaps | Editor build and affected Automation names, physical assertions, reset and isolation evidence |
| E: real workflows | Registered smoke matrix and affected acceptance flows | Final-candidate real E2E and owned-process shutdown; content-specific gates recorded separately |
| F: documentation | Tested examples in `docs/how-to/write-tests.md` and `tests/README.md` | Links, examples, and evidence limits checked; final inventory reconciled |

Slices B and C can proceed independently when they do not edit shared files.
Slices D and E may use a short-lived integration branch and one reusable Windows worktree.
Freeze the combined source before shared expensive checks. Build once when the same build covers the candidate.
After a correction, rerun affected checks and assess shared behavior before merge.

Applicable commands, from the repository root:

```text
uv run pytest -q
uv run pytest tests/protocol/unit tests/protocol/integration -q
uv run pytest tests/parity -q
uv run ruff check src scripts tests
uv run mypy src tests
uv run python scripts/check_docs.py
uv run python scripts/run_all_tests.py
uv run python scripts/run_e2e.py --suite all
```

Choose the applicable subset. The full runner already overlaps the separate commands.
Use the actual supported host/profile flags from the selected runner revision.
GitHub Actions remain static-only. A new pytest gate or hardware runner needs a separate maintainer decision.

## 13. Evidence and closure

Keep existing per-invocation YAML reports and logs.
Record commit and dirty diff, command, host/build/device, seed where relevant, selected and completed identities, result, and duration.
Map runner states to the workflow's passed, failed, skipped, and blocked claims without concealing timed-out, interrupted, or not-run stages.
Keep correctness, startup smoke, parity, physical response, and learning quality as separate claims.

Close #29 only when the final tracked-file inventory is reconciled, confirmed defects are repaired,
changed assertions have independent oracles, bounded regression demonstrations are recorded,
and all required native and E2E checks refer to the final candidate.
The updated authoring guide must use examples that actually exist in the improved suite.
The maintainer reviews the remaining limitations and authorizes merge under the repository workflow.
Delete finished branches only after merge when no development still needs them.

## Limitations

This proposal is based on the Issue, repository tree, contribution guides, and representative source inspection.
It is not the completed file-by-file disposition required by workstream 0.
This documentation change does not run Python behavior tests, build UE, execute E2E, or establish learning quality.
PR #33 has its own outstanding Windows gates.
Future vision inputs and independent first-use feature acceptance remain under their respective designs and Issues.

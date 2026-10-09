# General platform release roadmap

Status: proposed release plan, updated 2026-10-09.
The release version and date are not assigned.

## Release goal

Release a reusable platform for robot policy training in Unreal Engine and
policy deployment into independent UE projects. An external developer must be
able to add supported Robots, Tasks, and inputs through documented interfaces,
without maintaining a fork of framework internals.

The release includes the Python package, UE plugin, reference examples,
documentation, and validation evidence. A public game demo can use the platform
later. Its gameplay, content, interaction, and presentation choices are separate
product decisions and are not requirements in this roadmap.

Completing Issue #41, PR #50, and Issue #8 does not by itself establish release
readiness. Extension contracts, independent reproduction, packaged runtime,
distribution checks, and developer documentation must also pass.

[RFC 0001](../rfcs/0001-developer-workflow.md) remains the accepted developer
workflow. Reuse working interfaces. Write a focused RFC before changing a public
Task, input-provider, timing, Run, or artifact contract.

## Meaning of a general platform

Generality means reusable and tested extension paths within a declared support
matrix. It does not mean automatic support for every robot, sensor, Python
function, UE version, or game world.

| Layer | Responsibility | Required boundary |
|---|---|---|
| Framework and runtime | Session timing, physics stepping, reset, layouts, plans, inference, lifecycle | No dependency on a particular Robot, Task, map, or application objective |
| Robot integration | Topology, actuators, reference pose, input bindings | Add supported Robots through declarations and generic interfaces |
| Input provider | State or sensor collection, units, frames, sampling, reset | The same policy input contract in training and deployment |
| Task package | Commands, observations, rewards, termination, events, curriculum, evaluation | Use the shared runtime rather than a copied simulation loop |
| Game integration | Application commands, scene binding, policy lifecycle | No Python trainer required for deployed inference |
| Reference examples | Demonstrate and test public interfaces | No undocumented framework modifications |

Minimal Direct Tasks and Manager-composed Tasks use the same step/reset path.
Export is a separately checked capability. Arbitrary Python observation/action
code is not exportable unless a supported implementation and evidence establish it.

## Initial support boundary

Start with the documented Windows x64 and UE 5.8 setup. Use CartPole and PhantomX
as distinct Robot examples. Declare the supported input providers, static-ground
scenes, control timing, Task entry styles, and export path.

Freeze exact tested versions before the release candidate. Keep Python-only
checks available without UE. Unsupported combinations must be visible before
an expensive training run. New Robot or sensor support follows the same admission
and validation procedure; it does not inherit support from interface compatibility.

The input design must permit future providers without a Task-specific transport
path. A particular future sensor implementation is not required for this release.
Broader environments, distributed training, new solvers, and game-specific
features remain separate proposals.

## Current baseline

This snapshot records state on 2026-10-09, not release certification.

| Work | Recorded state | Remaining platform gate |
|---|---|---|
| Host configuration | Issue #5 closed | Reproduce setup and diagnostics on the release host matrix |
| External Tasks | Issue #6 closed | Audit extension documentation and repeat the independent installed-package path |
| Saved Runs | Issue #7 closed | Verify continuation, evaluation, and export on the release candidate |
| Test quality | Issue #29 closed | Run applicable suites against the final source and artifacts |
| Runtime performance | PR #45 merged | Measure supported training and packaged deployment separately |
| Evaluation | PR #47 Ready, not merged | Integrate reviewed metrics without equating survival and tracking |
| Learned reference | Issue #41 open, PR #50 active | Select a validated policy and document its operating envelope |
| Deployment | Issue #8 open | Held-out scene and actual packaged-runtime acceptance |
| Distribution | Preliminary inventory exists | Complete the actual release bundle and independent installation checks |

PR #48 is currently a dependency of #50. Consolidate the final code without
breaking active dependencies or losing experiment evidence. Reconcile overlapping
or obsolete Issue #40 scope before using it as a release checklist.

## M0 Freeze contracts and the support matrix

For code changes, complete a detailed architecture design before implementation.
Include source-level gaps, responsibilities, interfaces, data flow, configuration,
lifecycle, failures, compatibility, tests, and staged rollout. Use the
[platform extension design](../design/platform-extension-architecture.md) and
[proposed input RFC](../rfcs/0002-shared-input-providers.md) for the current audit.

Audit Robot, Task, input, Run, artifact, and game-control interfaces. Classify each
finding as working behavior, a documentation gap, a defect, or a missing extension.
Avoid broad rewrites where the current interface meets the requirements.

Publish the supported host versions, Robot examples, Task styles, input providers,
control timing, maps, and export capabilities. Map each supported combination to
its tests. Define compatibility and migration expectations for this Early-Stage
release. Preserve the shared Session clock and completed-window semantics.

Exit: reviewed contracts and support matrix, with an accepted RFC for each public
contract change. Assign an owner and evidence location to every release gate.

## M1 Prove the extension paths

Verify independent installation and registration of external Tasks. Demonstrate
both Direct and Manager composition, a reward change, and configuration overrides
outside the repository checkout. Retain early capability checks for export.

Verify Robot integration through declarations and generic UE interfaces. The
CartPole and PhantomX examples must not require Robot-name branches in the Worker
or policy controller. Document which native integration work a new provider needs.

For each supported input provider, define name, shape, type, units, frame,
configuration, sampling time, reset behavior, missing-data behavior, and deployment
binding. Keep ground geometry separate from the sensor that queries it. Different
training and deployment maps must preserve the policy input contract.

Prove provider reuse with an independent extension example and distinct static
scene bindings. Check configuration, reset, isolation, capability reporting, and
export/runtime behavior where applicable. An unsupported provider must produce a
useful diagnostic rather than silently produce a different observation.

Exit: a developer can extend the declared interfaces without changing core
application dispatch. Tests prevent concrete examples from becoming core dependencies.

## M2 Complete the reference training workflow

Use Issue #41 for the learned reference and the existing public workflow for
host setup, training, saved-Run continuation, evaluation, and export. Complete the
current PR #50 experiment before changing its parameters.

Keep exact source, resolved YAML, seeds, checkpoints, transition counts, optimizer
budgets, physical time, and per-behavior results. Separate runtime correctness,
survival, and learned quality. Select feasible operating bounds from evidence.

For the selected learned candidate, use independent training and evaluation seeds
as required by #41. Freeze behavior thresholds before held-out acceptance tests.
Keep failures and limitations. Do not tune the threshold to the acceptance result.

Exit: reproducible reference Runs and policies pass their declared behavior
protocol and can be restored, evaluated, and exported through the public commands.

## M3 Prove independent game deployment

Use Issue #8. Integrate the artifact into a separate target UE project or scene
through public interfaces. The deployment path must not depend on training
processes, private checkout paths, or a specific reference application.

Check units, frames, terrain queries, joint defaults, command timing, history,
reset, lifecycle, and missing-input behavior. Run same-input numerical parity and
separate closed-loop behavior tests. Test supported timing variation, stale
commands, faults, stop/restart, unload/reload, and repeated reset.

Use held-out static-ground starts and scene bindings. Preserve the distinction
between a reference controller's learning quality and the runtime's correctness.
Test the actual packaged UE executable, not only the Editor.

Exit: a named artifact works in the documented independent deployment procedure,
with reproducible runtime, scene, and supported-behavior evidence.

## M4 Validate the frozen release candidate

Freeze source, assets, policies, dependencies, versions, and reference maps.
Run applicable Python, native, E2E, parity, package, and lifecycle checks against
that exact candidate. Recheck affected gates after repairs.

Define target hardware, workload, robot count, frame-time and memory budgets,
and soak duration before final measurements. Report training throughput separately
from deployed frame time. Include latency percentiles, missed control windows,
system/GPU memory, and repeated-reset stability where applicable.

Use an independent Windows setup and a developer who did not write the changes.
They must install the platform, create an external Task, change one reward, train
the reference, restore a Run, export, and deploy through the documentation.
Record failures and corrections. If the host or tester is unavailable, the gate
remains blocked. Repeating the original setup does not replace this evidence.

Exit: all required candidate checks pass, the independent procedure completes,
and no release-blocking defect remains.

## M5 Prepare verified distribution

Complete the [release inventory](../release-readiness.md) for the actual source
and binary bundles. Verify Robot assets, policy artifacts, media, dependencies,
notices, historical content, and the documented security-reporting channel.
Keep source-asset rights separate from packaged-game rights. A code license does
not clear every content file. UE remains a separately acquired development
prerequisite; do not distribute its installation.

Provide installation and extension guides, supported versions, reference training
recipes, artifact/deployment instructions, known limitations, and release notes.
Test downloads and instructions using the candidate package. Record the complete
file list and exact source/policy versions. Destructive history changes and new
security settings require separate authorization.

Exit: packages, reference examples, documentation, and evidence are ready for a
separate maintainer decision to tag and publish. Readiness does not itself perform
publication or change repository visibility.

## Dependencies and tracking

M0 and the current M2 experiment can proceed together. M1 addresses measured
extension gaps. Final M3 acceptance uses a selected M2 artifact. M4 consumes the
combined frozen result. Start documentation and the distribution inventory early;
finish M5 against the accepted candidate.

Use [Issue #51](https://github.com/zpyc1oud/EmbodiedUE/issues/51) as the release tracker.
Reuse [#41](https://github.com/zpyc1oud/EmbodiedUE/issues/41) for reference learning
and [#8](https://github.com/zpyc1oud/EmbodiedUE/issues/8) for deployment.
[Issue #52](https://github.com/zpyc1oud/EmbodiedUE/issues/52) tracks extension and
shared input contracts. [Issue #53](https://github.com/zpyc1oud/EmbodiedUE/issues/53)
tracks candidate validation and distribution. Existing completed work is a baseline to
verify, not a reason to recreate #5, #6, #7, or #29.

Each execution Issue records scope, dependencies, deliverables, tests, and its
exit condition. Implementation PRs link to those Issues. Keep application-specific
ideas outside platform acceptance until their scope is separately agreed.

## Release-ready decision

The tracking Issue must link evidence for supported extension, reproducible
training/Run operations, independent game deployment, packaged execution,
independent developer reproduction, and cleared distribution. Every required
gate must pass. Open blocking defects, unresolved bundle rights, and unexecuted
required gates prevent the release-ready claim.

A successful reference demo alone cannot establish a general platform. Platform
unit tests alone cannot establish a working deployed controller. Require both
reusable interfaces and complete reference paths, with explicit support bounds.
Set the version and date after the candidate and publication decisions are ready.

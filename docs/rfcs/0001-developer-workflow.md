# RFC 0001: Developer workflow for the next release

- Status: Accepted
- Accepted: 2026-10-04
- Created: 2026-10-04
- Baseline: `42fae6dd404b8a5a74110113cdde251c7ec069b5`
- Related plan: [Next release roadmap](../roadmap/next-release.md)

## Summary

Make external Task development and run-to-game deployment one supported procedure.
Keep the existing simulation and training core.
Change three boundaries: external packages, Run resolution, and deployment into a target game.

A minimal Task entry point and advanced Manager composition must share execution behavior.
The entry style alone does not determine export capability.

## Problem

At the recorded baseline, training works on the maintainer's Windows setup.
External developers encounter repository-relative resources, built-in registration assumptions, repeated host configuration, and different configuration-restoration paths.

Command renaming alone cannot solve these problems.
A complete engine rewrite would remove working behavior and increase the required physics validation.

For example, baseline training resolves a resume checkpoint but builds configuration from current defaults.
Playback and export use saved Run configuration.
Existing objective checks and curriculum/random-state restoration must remain.
They do not replace complete configuration recovery.

## Goals

- Create and run a supported Task in an independent Python package.
- Let a beginner change one reward without knowledge of the complete Manager or plan compiler.
- Keep Task semantics consistent across continuation, evaluation, and export.
- Define target-game requirements and tests before a deployment-support claim.
- Give existing users an explicit migration path.

## Non-goals

This RFC does not replace Chaos, the synchronous Step protocol, the generic Robot runtime, or the training algorithm.
It does not promise arbitrary Python-to-C++ export, arbitrary Robot topology, or arbitrary dynamic-ground support.
The [roadmap](../roadmap/next-release.md) defines the release boundary.

## Proposed user experience

### Configure and create

The developer selects the UE installation and host project once.
Commands use that host profile, with explicit command-line overrides when necessary.
Static checks run without UE.
Asset and physics checks launch UE when required.

A generator creates an external Task package with registration, a supported Robot reference, configuration, tests, and a runnable entry point.
The initial example has no required integration TODOs.

### Modify a Task

The default template uses deployable observation and action components.
The user changes a reward function or weight, examines a short rollout, and starts training.
Advanced users compose Managers and curriculum through the same runtime.

Custom Python observation and action logic remains available for research.
Initial checks state whether the Task supports training, evaluation, and export.
Unsupported export logic must be clear before a long run.

### Continue, evaluate, and deploy

Training produces a Run reference with Task identity, resolved configuration, checkpoints, and outcome.
Continuation, evaluation, and export accept that reference without manual reconstruction of its identity.

Continuation uses the original semantics.
Evaluation selects explicit conditions without changes to the saved training configuration.
Export uses the actual trained plans and network.

The developer binds the policy to a supported Robot asset and target scene.
Compatibility checks detect interface and timing differences.
Behavior tests establish whether the policy works in that scene.

## Design

### 1. Task packages own Task definitions

An external package supplies its Task identifier, configuration factories, Task factories, Robot semantics declarations, and resources.
Combine built-in registration with explicitly enabled external packages.
Reject duplicate identifiers.

Use one recommended discovery mechanism.
Python entry points are the proposed default, subject to a minimal package prototype.
Package loading executes Python code.
It is not a sandbox.

Include built-in configuration as package resources.
External packages supply their own resources.
Do not locate resources through the current directory.
UE assets still own mass, inertia, geometry, and joint limits.
Task declarations refer to those assets and define their control and observation use.

### 2. Two entry points share the runtime

Use the existing [DirectTask contract](../../src/uerl/core/direct/task.py):

- Minimal entry: a runnable template with supported defaults and small action, observation, reward, and termination customization points.
- Composed entry: explicit Manager and curriculum configuration for reusable advanced behavior.

Both entries use DirectEnv, Session, valid-Slot handling, the reset sequence, and the training adapter.
Task code must not take ownership of Worker processes, reset execution, or device lifecycle.

Supply only unambiguous defaults.
Units, joint mapping, gains, action scale, and timing come from explicit validated definitions.

Export capability belongs to the observation and action implementation.
Supported plans can export from either entry point.
A minimal template does not make arbitrary Python computations deployable.

### 3. One application service resolves run intent

Move reusable Run restoration from the CLI to an application-level resolver.
Keep typed configuration validation.
CLI commands parse arguments and present results.

The resolver accepts the operation, Task/Run source, host settings, and explicit overrides.
It returns resolved configuration, source information, and meaningful differences.
The first implementation determines exact type names.
This RFC does not define them as another public API.

| Intent | Configuration source | Change rule |
|---|---|---|
| New training | Task defaults, experiment preset, explicit overrides | Save the effective configuration |
| Continuation | Original run snapshot | Allow defined machine/output/budget changes; reject silent Task changes |
| Warm start | Out of scope | No warm-start mode; start fresh training or continue a compatible saved Run |
| Evaluation | Saved policy semantics plus explicit evaluation conditions | Preserve the original training configuration |
| Export | Saved trained configuration and actual plans | Validate sufficient metadata; use online initialization when necessary |

Reward or timing changes can preserve dimensions but change semantics.
Missing historical configuration requires explicit recovery input or a clear statement that faithful continuation is unavailable.
It must not silently use current defaults.

Use existing Run records.
Do not add a parallel experiment database or general migration framework.

### 4. Deployment preserves policy semantics

Continue to export the observation plan, ONNX, action plan, and runtime/timing metadata.
Deployment must not rebuild them from current Task defaults.

Keep observation order, units, frames, normalization, action mapping, actuator meaning, and history-state behavior consistent.
Use existing binding and physics gates.
Add a compatibility field only when a supported case needs it for a decision.

Training uses request-driven fixed steps with training substepping disabled.
Gameplay uses synchronous substeps and a completed solver clock.
Compare actual observation and action timing.
Do not copy physics configuration files as proof of equivalent timing.

Map integration defines ground selection, ignored objects, spawn/reset locations, and command units and ranges.
Training uses its terrain whitelist or Slot query channel.
Deployment uses WorldStatic queries.
Each supported scene must produce the intended policy input.

The host specifies a tested response and restart path for overruns, stale commands, and faults.
The operation `StopPolicy` stops inference while physics continues.
It is not a complete physical fallback.

## Alternatives and tradeoffs

- **CLI cleanup only:** low cost, but leaves restoration differences and repository-resource dependencies.
- **A second direct runtime:** duplicates reset, masking, timing, and error semantics. Reject this option.
- **Mandatory deployment plans for all Tasks:** simplifies deployment but prevents useful research-only computations. Keep export capability explicit instead.
- **Full framework rewrite:** increases migration and physics-validation costs without a demonstrated need. Keep the working core.

The design adds a small public extension contract and tests for both entry styles.
It avoids two execution engines.
Use the first CartPole path to find missing boundaries before generalization.

## Compatibility and migration

Where practical, keep valid CLI use and route it through the shared resolver.
Document changed fields and their new source.
Put host paths in the local profile, not in shared Task defaults.

Move an internal CartPole Task through the external-package contract first.
Then use the same contract for PhantomX.
Supply a real Task example before and after migration.
Keep existing DirectTask subclasses and Managers where their contracts remain valid.

Continue compatible checkpoints with saved configuration.
Require explicit recovery input for missing metadata.
Changed Task semantics require a new experiment or a specifically supported conversion.
Keep the old Run and checkpoint.

Keep the artifact format unless a concrete requirement prevents this.
Use existing import and binding checks to determine support.
Explain when a policy requires re-export or new training.
Do not guess its observation layout.

Document each deprecated entry point, its replacement, and the planned removal version.
Do not add indefinite wrappers without a supported caller.
Select a release number after the public compatibility decision.
Keep package, plugin, and release metadata consistent.
Implementation size alone does not justify the name 2.0.

## Validation

Use the existing boundaries in the [test guide](../../tests/README.md).
For practical test instructions, see [Write tests](../how-to/write-tests.md).
Examine observable behavior:

1. Install a built package and create a runnable Task outside the repository.
2. Compare equivalent minimal and composed Tasks, including reset and invalid-Slot behavior.
3. Change current defaults and make sure that continuation still uses saved Run semantics.
4. Compare fixed inputs through Python/C++ plans, inference, and action output.
5. Compare fixed-action physical response, then closed-loop behavior in the same scene.
6. Evaluate held-out target scenes and supported timing and load conditions.

Existing numerical tolerances apply to their defined cases, not complete physical trajectories.
Keep the benchmark protocol as separate evidence.
After pilot runs, freeze learning-quality and behavior thresholds before release measurements.

## Rollout

First combine host setup, an external CartPole Task, Run operations, and deployment into one complete path.
Keep each change runnable and reviewable.
Then exercise PhantomX curriculum, variable intervals, and terrain through the same interfaces.

After RFC review, track execution in theme Issues under one release Milestone.
Update this RFC after a design decision changes.
Update user guides with their implementations.
The roadmap defines scope, and Issues record progress.

A release candidate freezes interfaces and migration guidance.
Then an independent Windows setup runs the procedure.
Publish supported versions, reference results, known limitations, and upgrade instructions with the release.

## Implementation details to resolve

- Validate entry-point discovery and the smallest public Task interface with the external CartPole prototype.
- Freeze the supported Windows/UE/Python combination and permitted continuation overrides.
- Define behavior thresholds and estimate remaining work after the complete path and pilot measurements.

The maintainer accepted the release goal and core design on 2026-10-04.
The listed details still require validation within this accepted design record.
Design acceptance does not mean implementation is complete.

## References

- [Current architecture](../architecture.md), [domain contracts](../../CONTEXT.md), and [deployment guide](../in-game-deployment-guide.md).
- [Isaac Lab task workflows](https://isaac-sim.github.io/IsaacLab/main/source/overview/core-concepts/task_workflows.html): minimal and composed workflows need not have the same user-facing complexity.
- [Rust RFC template](https://github.com/rust-lang/rfcs/blob/main/0000-template.md): motivation, user examples, reference design, alternatives, and unresolved decisions.
- [NumPy 2.0 API cleanup proposal](https://numpy.org/neps/nep-0052-python-api-cleanup.html): connect interface changes to explicit migration guidance.

# RFC 0001: Developer workflow for the next release

- Status: Draft
- Created: 2026-10-04
- Baseline: `42fae6dd404b8a5a74110113cdde251c7ec069b5`
- Related plan: [Next release roadmap](../roadmap/next-release.md)

## Summary

Make external Task development and run-to-game deployment a coherent supported workflow. Retain the existing simulation and training core. Change three boundaries: Task packages versus the framework, CLI commands versus run resolution, and trained policy semantics versus the target game.

A minimal Task entry point and advanced Manager composition must share execution behavior. A Task's entry style does not determine whether it can export.

## Problem

The current training path works on the maintainer's Windows setup. External developers still encounter repository-relative resources, built-in registration assumptions, repeated machine configuration, and different configuration-restoration paths.

These are user-facing workflow problems. Renaming commands alone will not fix them. A full engine rewrite would discard working behavior and expand the physics-validation burden.

For example, the training CLI resolves a resume checkpoint and builds configuration from current defaults, while playback/export use saved run configuration. Existing objective checks and restored curriculum/random state should remain, but they do not replace full configuration recovery.

## Goals

- Create and run a supported Task in an independent Python package.
- Let a beginner change one reward without learning the complete Manager or plan-compiler structure.
- Preserve Task semantics across continuation, evaluation, and export.
- Define what a target game must provide and test before claiming deployment support.
- Give existing users an explicit migration path.

## Non-goals

This RFC does not replace Chaos, the synchronous Step protocol, the generic Robot runtime, or the training algorithm. It does not promise arbitrary Python-to-C++ export, arbitrary Robot topology, or arbitrary dynamic-ground support. The [roadmap](../roadmap/next-release.md) defines the release boundary.

## Proposed user experience

### Configure and create

The developer selects the UE installation and host project once. Commands reuse that machine profile, with explicit command-line overrides when needed. Static checks run without UE; asset or physics checks launch it when required.

A project generator creates an external Task package with registration, a supported Robot reference, configuration, tests, and a runnable entry point. The initial example has no required integration TODOs.

### Modify a Task

The default template reuses deployable observation/action components. The user edits a reward function or weight, checks a short rollout, and starts training. Advanced users compose Managers and curriculum through the same runtime.

Custom Python observation/action logic remains available for research. Initial checks state whether the Task supports training, evaluation, and export. Unsupported export logic is explained before a long run.

### Continue, evaluate, and deploy

Training produces a run reference pointing to Task identity, resolved configuration, checkpoints, and outcome. Continuation, evaluation, and export accept that reference without requiring the developer to reconstruct its identity.

Continuation uses the original semantics. Evaluation selects explicit conditions without editing the saved training configuration. Export uses the actual trained plans and network.

The developer binds the policy to a supported Robot asset and target scene. Compatibility checks identify interface or timing mismatches. Behavioral tests establish whether the policy works in that scene.

## Design

### 1. Task packages own Task definitions

An external package supplies the Task identifier, configuration and Task factories, Robot semantics declarations, and resource access. Compose built-in registration with explicitly enabled external packages. Reject duplicate identifiers.

Use one recommended discovery mechanism. Python entry points are the proposed default, subject to a minimal packaging prototype. Loading an enabled package executes Python code; it is not a sandbox.

Package built-in configuration as package resources. External packages provide their own resources. Do not infer resources from the current working directory. UE assets still own mass, inertia, geometry, and joint limits; Task declarations reference them and define their control/observation use.

### 2. Two entry points share the runtime

Build on the existing [DirectTask contract](../../src/uerl/core/direct/task.py):

- Minimal entry: a runnable template with supported defaults and small action, observation, reward, and termination customization points.
- Composed entry: explicit Manager and curriculum configuration for reusable advanced behavior.

Both use the existing DirectEnv, Session, valid-Slot handling, reset sequence, and training adapter. Task code must not acquire Worker, process, reset, or device-lifecycle responsibilities.

Fill only unambiguous defaults. Units, joint mapping, gains, action scale, and timing come from explicit validated definitions.

Exportability is a capability of the observation/action implementation. Supported plans can export from either entry point. Arbitrary Python computations do not become deployable merely because their Task uses the minimal template.

### 3. One application service resolves run intent

Move reusable run restoration out of the CLI into an application-level resolver. Keep the existing typed configuration validation. CLI commands parse arguments and present results.

The resolver accepts a run intent, Task/run source, machine settings, and explicit overrides. It returns resolved configuration, source information, and meaningful differences. Exact type names are settled by the first implementation, not treated as an additional public API here.

| Intent | Configuration source | Change rule |
|---|---|---|
| New training | Task defaults, experiment preset, explicit overrides | Save the effective configuration |
| Continuation | Original run snapshot | Allow defined machine/output/budget changes; reject silent Task changes |
| Warm start | New experiment plus selected prior weights | State which optimizer, normalization, and curriculum state is restored |
| Evaluation | Saved policy semantics plus explicit evaluation conditions | Preserve the original training configuration |
| Export | Saved trained configuration and actual plans | Validate sufficient metadata; use online initialization when necessary |

A dimension-preserving change to rewards or timing is still a semantic change. Missing old configuration requires explicit input or a clear inability to continue faithfully; it must not silently fall back to current defaults.

Reuse existing run records. Do not introduce a parallel experiment database or a general migration framework.

### 4. Deployment preserves policy semantics

Continue exporting the observation plan, ONNX, action plan, and runtime/timing metadata. Deployment must not rebuild these from current Task defaults.

Keep observation ordering, units, frames, normalization, action mapping, actuator meaning, and history-state behavior consistent. Reuse existing binding and physics gates. Add compatibility fields only when a supported case needs them to make a decision.

Training and gameplay intentionally use different scheduling mechanisms: request-driven fixed steps with training substepping disabled, versus synchronous substeps and a completed solver clock in the game. Compare actual observation and action timing, rather than copying physics configuration files.

Map integration defines eligible ground, ignored objects, spawn/reset locations, and command units/ranges. Training's terrain whitelist or Slot query channel and deployment's WorldStatic queries must produce the intended policy input in each supported scene.

For overruns, stale commands, or faults, the host specifies a tested response and restart path. `StopPolicy` stops inference while physics continues; it is not a complete physical fallback.

## Alternatives and tradeoffs

- **CLI cleanup only:** inexpensive, but leaves restoration and repository-resource coupling unresolved.
- **A second direct runtime:** gives apparent simplicity but duplicates reset, masking, timing, and error semantics. Reject this option.
- **All Tasks must compile to deployment plans:** simplifies the deployment promise but prevents useful research-only computations. Keep export capability explicit instead.
- **Full framework rewrite:** increases migration and physics-validation costs without demonstrating a need. Retain the working core.

The proposed design adds a small public extension contract and tests for two entry styles. It avoids maintaining two execution engines. A first CartPole path should expose missing boundaries before generalizing them.

## Compatibility and migration

Preserve valid existing CLI use where practical and route it through the shared resolver. Document changed fields and their new source. Import machine paths into the local profile rather than embedding them in shared Task defaults.

Move an internal CartPole Task through the external-package contract first, then PhantomX. Provide an actual before/after Task example. Keep current DirectTask subclasses and Managers where their contracts remain valid.

Continue compatible checkpoints with saved configuration. Missing metadata requires explicit recovery inputs. Changed Task semantics use a new experiment or a specifically supported conversion. Preserve the old run and checkpoint.

Keep the artifact format unless a concrete required change makes that impossible. Use the existing import/binding checks to determine support. Explain when an old policy needs re-export or retraining rather than guessing its observation layout.

Document any deprecated entry point, its replacement, and the planned removal version. Do not add indefinite wrappers without a real supported caller.

Choose the release number after deciding the public compatibility promise. Keep package, plugin, and release metadata consistent. A large implementation effort alone does not justify naming the release 2.0.

## Validation

Use existing test seams from the [test guide](../../tests/README.md). Check observable behavior:

1. Install a built package and create a runnable Task outside the repository.
2. Compare equivalent minimal/composed Tasks, including reset and invalid-Slot behavior.
3. Change current defaults and verify continuation still uses the saved run semantics.
4. Compare fixed inputs through Python/C++ plans, inference, and action output.
5. Compare fixed-action physical response, then same-scene closed-loop behavior.
6. Evaluate held-out target scenes and supported timing/load conditions.

Existing numerical parity tolerances apply to their defined cases, not whole physical trajectories. The benchmark protocol remains a separate supporting document. Freeze learning-quality and behavioral thresholds after pilots, before release measurements.

## Rollout

First connect host setup, an external CartPole Task, unified run operations, and deployment into one complete path. Keep each change runnable and reviewable. Then exercise PhantomX curriculum, variable intervals, and terrain through the same interfaces.

After RFC review, track execution through theme Issues under one release Milestone. Update this RFC when a design decision changes. Update user guides with their implementations. The roadmap owns scope, while Issues own progress.

A release candidate freezes the interfaces and migration guidance, then runs on an independent Windows setup. Publish supported versions, reference results, known limitations, and upgrade instructions with the release.

## Unresolved decisions

- Confirm the initial release goal and static-ground deployment boundary.
- Validate entry-point discovery and the smallest public Task interface with the external CartPole prototype.
- Freeze the supported Windows/UE/Python combination and the explicitly allowed continuation overrides.
- Establish behavioral thresholds and estimate remaining work after the complete path and pilot measurements.

The overall document structure is agreed; these technical details remain proposals until reviewed.

## References

- [Current architecture](../architecture.md), [domain contracts](../../CONTEXT.md), and [deployment guide](../in-game-deployment-guide.md).
- [Isaac Lab task workflows](https://isaac-sim.github.io/IsaacLab/main/source/overview/core-concepts/task_workflows.html): minimal and composed workflows need not have the same user-facing complexity.
- [Rust RFC template](https://github.com/rust-lang/rfcs/blob/main/0000-template.md): motivation, user examples, reference design, alternatives, and unresolved decisions.
- [NumPy 2.0 API cleanup proposal](https://numpy.org/neps/nep-0052-python-api-cleanup.html): connect interface changes to explicit migration guidance.

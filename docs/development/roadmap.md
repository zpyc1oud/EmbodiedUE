# Development roadmap

Status: proposed development plan. Baseline: `42fae6dd404b8a5a74110113cdde251c7ec069b5`.

The maintainer has completed the full training workflow on one Windows machine. The next goal is to let another developer install the project, train a reference Task, change its behavior, and deploy a policy without modifying framework internals.

This plan covers developer experience, architecture changes, and release scope. The [training benchmark design](training-benchmarks.md) defines measurements. The [training and deployment plan](train-deploy-parity.md) defines compatibility and closed-loop validation. Current behavior remains documented in [architecture](../architecture.md) and [CONTEXT](../../CONTEXT.md).

## Architecture to retain

- Python owns Task mathematics, rewards, termination, curriculum, and PPO. UE owns physics, reflected Topology, state collection, and physical execution.
- Keep the typed configuration resolver and immutable Session contract. Generate the Worker projection from resolved configuration.
- Keep the existing Robot and Environment Provider factories. New supported Robots should reuse generic runtime code.
- Keep one synchronous request in flight until measurements justify another design. Ambiguous Step failures must not silently replay actions.
- Preserve process ownership: closing a launched Worker and detaching from an existing Editor are different operations.
- Preserve the shared observation/action plans used by training and deployment. Keep policy inference independent of the training process.
- Extend existing profiling and dependency tests rather than creating parallel systems.

## Priority 1: consistent configuration and continuation

### DEV-01: one application-level run resolver

Problem: training builds configuration through a different path from playback and export. Reusable restoration logic currently lives in the CLI layer.

Change:

- Move reusable run resolution into an application service. CLI commands handle arguments and presentation.
- Express the run intent explicitly: new training, continuation, evaluation, or export.
- Separate machine settings, Task configuration, experiment overrides, and the saved resolved configuration.
- Show the effective values and their source. Report semantic changes before UE starts.

Acceptance: equivalent run references restore the same Robot semantics, Task parameters, observation/action layout, and timing across commands. Output paths and machine settings can change without altering the experiment.

### DEV-02: continuation versus warm start

Problem: `train --resume` resolves a checkpoint but then builds from current Task defaults. Playback/export restore `resolved_config.json`. Existing objective checks and curriculum/random-state restoration are useful but do not replace configuration restoration.

Change:

- Continue from the saved run configuration by default.
- Allow explicit machine/output changes and additional training budget.
- Treat changes to rewards, observations, actions, timing, or network structure as a new experiment or a specifically supported conversion.
- Define warm start separately: document which weights, optimizer state, normalization, and curriculum state are restored.
- Handle supported older saved runs explicitly when an actual schema change requires it. Do not create a general migration framework in advance.

Acceptance: editing current Task defaults does not silently change a continued run. Tests include semantic changes with unchanged tensor dimensions. Supported continuation preserves the existing state-restoration behavior.

Dependency: DEV-01.

## Priority 2: installation and normal operations

### DEV-03: one machine profile

Store the selected UE executable, host project, and relevant device preferences once. Keep explicit command-line paths as overrides. Reuse the same selection in training, playback, and export. Multiple UE installations require a deterministic, visible choice.

Acceptance: switching Tasks or terminals does not require re-entering UE paths. An invalid path fails before starting a long run and names the corrective action.

### DEV-04: extend existing diagnostics

Reuse `check` and deployment preflight. Separate declaration checks, host checks, and an optional startup probe. Check required LFS content, UE/project paths, build tools, plugins, Python dependencies, device availability, and output access where relevant.

Acceptance: declaration checks do not launch UE. Errors identify the failed object, cause, and next action. A requested training device is not silently replaced. A startup probe cleans up only processes it owns.

### DEV-05: useful run selection and status

Extend existing run directories, `runs`, and `latest`. Distinguish the latest run from the latest run with a recoverable checkpoint. Record normal completion, interruption, and failure. Infer Task identity from run metadata where available; an explicit conflicting Task must fail.

Acceptance: a newly created run without a checkpoint does not hide the latest recoverable run. A run reference is sufficient for operations whose required identity is already recorded. Partial failures leave actionable status.

### DEV-06: installable package resources

The configuration root and deployment installer currently depend on repository-relative locations. Package built-in configuration resources; let external Task packages supply their own resources. Move reusable installer logic into an importable package module, keeping scripts as thin entry points.

Acceptance: build a wheel, install it outside the checkout, and discover/check built-in Tasks without editable installation or a repository working directory. UE assets remain a separately documented dependency.

Inspect the import graph before splitting dependencies. Delay training/video imports in the CLI where useful. A lightweight metadata command should not require video dependencies; do not remove Torch blindly from types that still use tensors.

## Priority 3: external Task development

### DEV-07: composed registration sources

Preserve existing registration types. Compose built-in registrations with explicitly enabled external packages for Tasks, Robot declarations, and resources. Choose one recommended discovery mechanism. Duplicate identifiers must produce a useful error.

Acceptance: an independently installed package supplies a Task without editing the framework registry. Built-in Tasks use the same public contract. Documentation explains that importing an extension executes its Python code.

Dependencies: DEV-06 for resources.

### DEV-08: minimal and composed entry points share one core

Isaac Lab offers both [Manager-based and Direct workflows](https://isaac-sim.github.io/IsaacLab/main/source/overview/core-concepts/task_workflows.html). Use that flexibility without maintaining two simulation loops.

Current `DirectTask` supports custom Task mathematics and Manager assembly. Build on it:

- Keep internal Managers for reusable actions, observations, rewards, termination, events, and curriculum.
- Supply a runnable external template with registration, Robot reference, configuration, entry point, and tests already connected.
- Let users first edit actions, observations, rewards, and termination through a small, documented interface.
- Use existing deployable observation/action components in the default template. A reward-only change should require one function or weight edit.
- Fill unambiguous defaults, such as an empty curriculum. Do not guess mass, inertia, joint order, units, action scaling, or timing.
- Offer advanced Manager composition through the same DirectEnv, Session, valid-Slot masks, reset behavior, and training adapter.

Task entry style and export capability are separate. A custom Python observation/action implementation can be a research Task without being exportable. Report training, evaluation, and export capability during initial checks. Do not promise arbitrary Python-to-C++ conversion.

Acceptance:

1. The generated minimal example runs outside the checkout.
2. Equivalent minimal and composed Tasks resolve identical observation/action semantics and episode behavior.
3. Changing a reward does not require C++ or framework changes.
4. Unsupported export logic is identified before a long training run.
5. No Task gains responsibility for Session, Worker, device lifecycle, or direct reset operations.

Dependencies: DEV-06 and DEV-07. Start with CartPole, then exercise PhantomX.

### DEV-09: configuration provenance

For new training, apply Task defaults, experiment presets, and explicit overrides in a documented order. For continuation, start from the saved configuration and allow only explicit supported changes. Machine settings provide launch details rather than Task defaults.

Acceptance: a developer can determine which input supplied a value and compare a proposed run with its parent without reading several implementation layers.

Dependency: DEV-01.

### DEV-10: explicit evaluation setup

Separate evaluation from playback and training curriculum. Specify simulated duration, episodes, terrain/command sets, and evaluation seeds. Preserve the original training configuration. Use inherited curriculum state only when requested by the evaluation design.

Acceptance: policies trained with different Slot counts can run the same evaluation protocol. Reports distinguish control steps, simulated seconds, and wall time.

## Priority 4: creation, diagnosis, and deployment

### DEV-11: Robot diagnostics

Provide reflected Topology inspection, selector/field validation, joint control tests, and reference-pose checks. Generate declaration drafts only from confirmed reflected information. Keep gains, scaling, and unsupported structures explicit.

Acceptance: an external developer can identify a deliberately wrong joint selector and test a supported Robot before designing a reward. Use an appropriately licensed minimal example.

### DEV-12: terrain preview

Reuse existing plane, heightfield, and box definitions. Preview a preset with a seed and level; expose generation, collision, and terrain-scan results. Creating a composition of supported primitives should not require new generic C++ code.

Acceptance: the preview and training refer to the same preset and sampled geometry. Unsupported geometry has a clear extension path.

### DEV-13: actionable performance reports

Extend `StageProfiler` with summaries and correlate client stages with policy inference, PPO update, UE physics, and state collection. Measure instrumentation overhead separately.

Acceptance: a benchmark can attribute a regression to a meaningful stage. Optimize the measured bottleneck; evaluate shared memory, asynchronous execution, or solver-quality changes only as separate experiments.

### DEV-14: deployment plan before execution

Extend existing preflight with the proposed file/settings changes, backup behavior, and repeat-execution checks. Resolve the Robot asset from the selected policy/run before considering current Task defaults. Keep the installer importable outside the checkout.

Acceptance: a repeated deployment does not duplicate settings; explicit overrides are checked; partial failures identify completed changes. A deployment controller dependency graph alone does not establish a minimal cooked plugin.

Offline export is a later, bounded improvement. First save the information needed to reconstruct the network, plans, normalization, and Robot Runtime. Keep online export when required metadata is absent.

## Documentation and developer onboarding

### DEV-15: organize by developer goal

Use these entry paths:

- First use: prerequisites, install/diagnose, first short training, locate results.
- Task development: edit one reward, edit an observation, create an external Task, add a Robot, change terrain.
- Operations: continuation/warm start, evaluation, recording, export, game deployment.
- Reference: configuration, CLI, supported Python/UE interfaces, artifact format.
- Design: ownership, timing, units/frames, randomness, extension contracts.
- Troubleshooting: setup, startup, stepping, training, export, deployment.

Every tutorial states its goal, prerequisites, inputs, steps, expected output, interpretation, common failures, and next step. Measure setup time before advertising it. A successful smoke run does not mean a policy has learned the Task.

Generate reference material from existing declarations when practical. Keep runnable tutorial snippets and their tests together. Do not require architecture study before first use.

Acceptance: a new developer installs the supported configuration, trains a reference Task, edits a reward, and runs an external Task without modifying framework internals. Record first-success time, commands used, failures, and time spent locating an error.

## Training and deployment work

The [parity plan](train-deploy-parity.md) expands six additional changes:

- DEV-16: explicit deployment compatibility data and comparison.
- DEV-17: diagnostic traces across Python and C++ boundaries.
- DEV-18: fixed-input, fixed-action, and same-scene closed-loop tests.
- DEV-19: authored-map query and reset contract.
- DEV-20: tested host behavior for overruns, stale commands, and policy faults.
- DEV-21: held-out map evaluation and a complete deployment tutorial.

These are development item identifiers, not existing GitHub issue or PR numbers. Implementation PRs should cover a coherent change with its tests and documentation.

## Sequence and planning estimate

| Batch | Items | Dependency and result | Initial effort |
|---|---|---|---|
| Daily training | DEV-01 to DEV-05 | Consistent continuation and host setup | 8-14 person-days |
| External development | DEV-06 to DEV-10 | Installable resources before external template | 12-20 person-days |
| Creation and diagnostics | DEV-11 to DEV-15 | Tools and tutorials delivered together | 15-25 person-days |

The original mainline estimate is 35-59 person-days, or about 44-74 with 25% contingency. It assumes continued access to a working Windows/UE host and the existing training workflow. Re-estimate after DEV-01/02 and the minimal-template prototype. It is not a release commitment.

DEV-16 to DEV-21, offline export, additional platforms, and precompiled distribution require separate estimates. First implement one diagnostic comparison to expose the actual deployment work. Do not hide that scope inside the original estimate.

## Release scope

A public beta should support a named Windows/UE/Python combination, an independently installable Task package, repeatable reference training, useful diagnostics, and documented evaluation/export/deployment.

A stable release should publish supported interfaces and artifact/checkpoint compatibility, reference results, known limitations, and a migration note for actual breaking changes. Complete the measured benchmark and target-map validation before claiming performance or deployment support.

Architecture changes need a user scenario, a bounded implementation, behavioral tests, and updated documentation. Physics and protocol changes require the relevant real-UE tests from the [test guide](../../tests/README.md).

## Isaac Lab attribution

Acknowledge the substantial architectural inspiration in project documentation. Identify copied or adapted files separately from independently implemented concepts. For actual reuse, record the upstream file/version, changes, and applicable notice in the existing third-party documentation. Preserve the relevant license terms; an acknowledgment is not a substitute.

Isaac Lab's [main license](https://github.com/isaac-sim/IsaacLab/blob/main/LICENSE) is BSD-3-Clause, but the specific reused version, component, and assets determine the obligations. Original EmbodiedUE material can retain Apache-2.0. Avoid implying affiliation or endorsement.

## Limitations

This is a source-informed development proposal, not new runtime evidence. One completed Windows training workflow does not establish cross-machine installation, packaging, every Robot, or benchmark results. Scope new abstractions around supported cases. Reuse existing metadata and validation; add a digest only if it avoids a materially expensive operation and changes the next action, as required by `AGENTS.md`.

## Source anchors

- [Run restoration](../../src/uerl/cli/run_config.py), [training CLI](../../src/uerl/cli/train.py), and [runner](../../src/uerl/training/runner.py).
- [Task contract](../../src/uerl/core/direct/task.py), [default registry](../../src/uerl/tasks/registry/defaults.py), and [template generator](../../src/uerl/cli/new.py).
- [Resource paths](../../src/uerl/core/config/paths.py), [deployment CLI](../../src/uerl/cli/deploy.py), and [profiling](../../src/uerl/core/direct/profiling.py).

# Documentation

EmbodiedUE helps game developers train physical robot controllers and deploy them in Unreal Engine.
Start with the [English README](../README.md) or [简体中文 README](../README.zh-CN.md).
Read [product direction](product-direction.md) for the audience and scope.

## Build your first game robot

Follow these stages in order for a new project.
Use a reference robot first, then customize its assets or Task.

| Stage | Read | Expected result |
|---|---|---|
| 1. Prepare the host | [Install and build](../README.md#getting-started), [host profile](how-to/ue-host-profile.md) | A built UE host and saved paths |
| 2. Prepare the robot | [Add a robot](how-to/add-a-robot.md) | Assets and a declaration with matching bodies, joints, and actuators |
| 3. Define the behavior | [Configuration](configuration.md), [external Tasks](how-to/external-tasks.md) | A registered Task with commands, observations, actions, and an objective |
| 4. Train and evaluate | [PhantomX guide](how-to/phantomx-robust-training.md) | A saved Run and measured behavior under declared conditions |
| 5. Connect the game | [Deployment](in-game-deployment-guide.md) | An imported policy with valid robot assets and gameplay commands |
| 6. Validate and show it | [Deployment validation](how-to/policy-deployment-validation.md), [record video](how-to/record-video.md) | Target-scene evidence and a recording of the selected policy |

Use the [how-to index](how-to/README.md) to choose a specific procedure.
For a failure, start with [troubleshooting](troubleshooting.md).

## Understand or extend the platform

| Question | Reference |
|---|---|
| Which component owns each responsibility? | [Architecture](architecture.md) |
| What do project terms and timing values mean? | [Domain glossary](../CONTEXT.md) |
| How do I contribute a change? | [Contributing](../CONTRIBUTING.md), [contribution workflow](contribution-workflow.md) |
| How do I write and run tests? | [Write tests](how-to/write-tests.md), [test suites](../tests/README.md) |
| How do I reuse expensive UE validation? | [Batch validation](how-to/batch-validation.md) |
| How do I add a plan operator? | [Operator admission](../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md) |
| How do I measure runtime cost? | [Runtime benchmark](how-to/runtime-benchmark.md) |

## Plans and design records

These pages explain planned work and design decisions.
Use the procedure guides above for current operation.
Check each design's status before treating it as an available feature.

- [Next release roadmap](roadmap/next-release.md): scope, stages, and release conditions.
- [RFC 0001: Developer workflow](rfcs/0001-developer-workflow.md): accepted design for user operations and architecture.
- [Task and runtime stabilization](design/task-runtime-stabilization.md): configured inputs and runtime verification.
- [Repository-wide test quality design](design/test-quality-implementation.md): proposed test improvements for Issue #29.
- [Testing audit](testing-audit-2026-10.md): findings and design comparisons.

Implementation progress belongs in the related GitHub Issues and PRs.

## Distribution and project policy

- [Release readiness](release-readiness.md)
- [Third-party notices and asset inventory](../THIRD_PARTY_NOTICES.md)
- [Asset publication plan](asset-publication-plan.md)
- [Security](../SECURITY.md)
- [Changelog](../CHANGELOG.md)

Videos in `docs/media/` show historical inference.
Generated Runs and private reference checkouts are not included as experiment evidence.

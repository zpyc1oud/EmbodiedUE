# Next release roadmap

Status: design accepted; implementation planning. Version number and release date are not assigned.

## Goal

Let an external developer install EmbodiedUE, create a Task outside this repository, train and evaluate a policy, and deploy it into a supported UE game scene.

The maintainer has completed the training workflow on one Windows machine. This release builds on that working path. The primary audience can write Python and build a UE project, but should not need to understand framework internals before changing a Task.

## Required outcomes

1. **Configure the host once.** Training, evaluation, and export share the selected UE installation and host settings. Failures identify the next corrective action.
2. **Develop an external Task.** A runnable minimal template and advanced Manager composition use the same runtime core. Export capability is explicit.
3. **Operate on a saved run.** Continuation, evaluation, and export recover the correct Task configuration and policy semantics from the run.
4. **Deploy with a defined contract.** Supported Robot assets, timing, observations, actions, and static-ground scenes pass both compatibility checks and behavioral evaluation.

The design and tradeoffs are in [RFC 0001](../rfcs/0001-developer-workflow.md). These are planned outcomes, not current feature claims.

## Release boundary

Start with the supported Windows/UE setup, CartPole, and PhantomX on static ground. Freeze the tested software versions before the release candidate.

Multi-platform support, distributed training, arbitrary dynamic scenes, and automatic conversion of arbitrary Isaac Lab Tasks are outside this release. Offline export is not required to establish the workflow.

## Stages

| Stage | Deliverable | Exit condition |
|---|---|---|
| Design | Reviewed RFC and a small interface prototype | Resolve the public Task boundary, run restoration rules, and deployment support scope |
| First complete path | CartPole through host setup, external Task, training, evaluation, and deployment | A developer follows the documented path without editing framework internals |
| Broader validation | PhantomX curriculum, variable control intervals, and terrain | The same public interfaces handle the supported locomotion workflow |
| Release candidate | Frozen interfaces, migration guide, reference results | An independent Windows setup completes the workflow; blocking defects are resolved |
| Release | Tagged code, supported-version matrix, change notes, known limitations | Installation, learning quality, continuation, and target-scene behavior meet the frozen criteria |

Measure throughput, learning quality, and deployment behavior separately. Use the benchmark protocol as supporting acceptance evidence; do not duplicate its experiment matrix here.

## Tracking

The accepted design is tracked by four theme Issues:

- [Host setup and diagnostics](https://github.com/zpyc1oud/EmbodiedUE/issues/5)
- [External Tasks and shared entry points](https://github.com/zpyc1oud/EmbodiedUE/issues/6)
- [Saved-run operations](https://github.com/zpyc1oud/EmbodiedUE/issues/7)
- [Game deployment validation](https://github.com/zpyc1oud/EmbodiedUE/issues/8)

Link implementation PRs to the relevant Issue. Group these under the release Milestone when it is created; the release number/date remain unassigned.

This page owns release scope. The RFC owns design decisions. Issues own execution status. The links above identify the execution records; progress is maintained there.

## Release decision

On a Windows machine not used for development, a non-author must be able to train the reference Task, change one reward, and deploy the policy into the named test scene using the documentation. Preserve the configuration, policy-quality results, performance baseline, and deployment results.

Publish the supported configuration and remaining limitations. Estimate remaining work after the first complete path, rather than committing to a date before the interface is proven.

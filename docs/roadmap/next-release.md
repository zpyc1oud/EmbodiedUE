# Next release roadmap

Status: design accepted, implementation planning.
The version number and release date are not assigned.

## Goal

A game developer can use EmbodiedUE to train a physical robot controller and deploy it into a supported Unreal game scene.
The developer can create a Task outside this repository and evaluate the trained behavior before deployment.
The [product direction](../product-direction.md) defines the audience and long-term goal.

The release must demonstrate a reusable path from robot assets to useful in-game behavior.
A showcase scene makes that path visible; the platform interfaces must also serve other supported projects.

The maintainer completed the training procedure on one Windows machine.
This release extends that working path.
The target reader can write Python and build a UE project.
A Task change should not require knowledge of framework internals.

## Required outcomes

1. **Configure the host once.**
   Training, evaluation, and export use the selected UE installation and host settings.
   Each failure identifies a corrective action.
2. **Develop an external Task.**
   A runnable minimal template and advanced Manager composition use the same runtime core.
   Export capability is explicit.
3. **Operate on a saved run.**
   Continuation, evaluation, and export recover the saved Task configuration and policy semantics.
4. **Deploy with a defined contract.**
   Supported Robot assets, timing, observations, actions, and static-ground scenes pass compatibility and behavior checks.

[RFC 0001](../rfcs/0001-developer-workflow.md) gives the design and reasons for its choices.
These outcomes are planned requirements, not current feature claims.

## Release boundary

Start with the supported Windows/UE setup, CartPole, and PhantomX on static ground.
Before the release candidate, freeze the tested software versions.

This release excludes multi-platform support, distributed training, arbitrary dynamic scenes, and automatic conversion of arbitrary Isaac Lab Tasks.
Offline export is not required for the procedure.

## Stages

| Stage | Deliverable | Exit condition |
|---|---|---|
| Design | Reviewed RFC and a small interface prototype | Resolve the public Task boundary, run restoration rules, and deployment support scope |
| First complete path | CartPole through host setup, external Task, training, evaluation, and deployment | A developer follows the documented path without editing framework internals |
| Broader validation | PhantomX curriculum, variable control intervals, and terrain | The same public interfaces handle the supported locomotion workflow |
| Release candidate | Frozen interfaces, migration guide, reference results | An independent Windows setup completes the workflow; blocking defects are resolved |
| Release | Tagged code, supported-version matrix, change notes, known limitations | Installation, learning quality, continuation, and target-scene behavior meet the frozen criteria |

Measure throughput, learning quality, and deployment behavior separately.
Use the benchmark protocol as acceptance evidence.
Keep its experiment matrix in that protocol rather than duplicate it here.

## Tracking

Four theme Issues track the accepted design:

- [Host setup and diagnostics](https://github.com/zpyc1oud/EmbodiedUE/issues/5)
- [External Tasks and shared entry points](https://github.com/zpyc1oud/EmbodiedUE/issues/6)
- [Saved-run operations](https://github.com/zpyc1oud/EmbodiedUE/issues/7)
- [Game deployment validation](https://github.com/zpyc1oud/EmbodiedUE/issues/8)

Link each implementation PR to the applicable Issue.
When a release Milestone exists, put the theme Issues in it.
The release number and date remain unassigned.

This page defines release scope.
The RFC records design decisions.
The linked Issues record execution status.

## Release decision

Use an independent Windows machine and a developer who did not write the implementation.
That developer must complete these steps through the documentation:

1. Train the reference Task.
2. Change one reward.
3. Deploy the policy into the named test scene.

Keep the configuration, policy-quality results, performance baseline, and deployment results.
Publish the supported configuration and remaining limitations.
Estimate the remaining work after the first complete path succeeds.
Do not promise a date before the interface is proven.

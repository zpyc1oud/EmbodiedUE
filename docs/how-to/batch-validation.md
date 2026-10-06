# Validate a feature batch

Use a persistent Windows worktree to validate several reviewed features together.
Keep the worktree, dependencies, and useful build caches between batches.
Use a new, short-lived integration branch for each batch.

The [contribution workflow](../contribution-workflow.md) defines review and merge requirements.
This procedure changes when shared UE checks run, not the required evidence.
A small change can still use a direct PR to `main`.

## Prepare the validation worktree

Use an authorized E-drive directory separate from the user's main checkout.
For example, keep the validation worktree at `E:/EmbodiedUE-validation`.
Keep reports in a separate directory for each invocation.
These example paths do not grant access to a computer or directory.

Before the first batch:

1. Confirm the selected UE installation, Python environment, host project, and required assets.
2. Confirm that the worktree has no unrecorded source changes.
3. Preserve existing Runs, checkpoints, assets, and logs.
4. Assign one operator to the validation worktree.

Stop active validation processes before changing the checked-out revision.
Do not build or run two batches in this worktree at the same time.
Keep feature development in separate branches and working directories.

Reuse dependencies only when they satisfy the selected lockfile and supported versions.
Reuse build caches through the normal incremental build process.
A cached binary alone does not prove that it contains the selected source changes.
Rebuild affected targets when source, build settings, engine versions, or dependencies change.
Use a clean build when there is evidence that incremental output is invalid.
Do not delete unrelated caches or user data as routine preparation.

## Review features before integration

Keep unrelated features in separate branches and PRs.
Each feature must pass its applicable early checks:

- Focused behavior and regression tests
- Ruff and Mypy for Python changes
- The default Python suite for code changes
- Installed-package checks when package discovery or resources change
- Documentation checks for changed guides and examples

Resolve blocking review findings before the feature enters the batch.
Do not defer all testing until integration.
Record required UE checks that remain pending for the batch.

A feature can enter an integration branch with its UE evidence pending.
It cannot enter `main` until the applicable acceptance evidence is complete.
If a host check is necessary to resolve a blocking design or review question, run it before integration.

## Assemble one candidate

1. Create a branch such as `integration/cartpole-deployment` from the current `main`.
2. Open a Draft integration PR targeting `main`.
3. List each included feature PR, its exact head, dependencies, and pending acceptance checks.
4. Retarget the selected feature PRs to the integration branch before merging them into it.
5. Integrate the reviewed features in dependency order.
6. Review conflict resolutions and run affected early checks on the combined code.

Keep the feature PRs and their review history linked from the integration PR.
Merging a feature into this temporary branch records integration, not final product acceptance.
Do not close its Issue solely because it entered the batch.

The current GitHub Actions workflow targets PRs to `main` and pushes to `main`.
A feature PR targeting an integration branch does not receive that workflow automatically.
Record its early-check results explicitly.
The integration PR receives the existing Ruff/Mypy checks.
Changing branch targets does not add UE tests to CI or alter repository protection rules.

## Freeze and validate the batch

Freeze the integration head before expensive host checks.
Record its commit, the `main` base, and any dirty diff.
If UE generates local configuration changes, keep that diff with the report.
Explain whether those changes are host-only or require a reviewed source change.

Use the [validation matrix](../contribution-workflow.md#4-select-validation-by-the-changed-behavior) to select the batch's required checks.
Combine shared checks once for the same candidate and host configuration.
Keep feature-specific checks that establish different behavior.

For example, one Editor build can support several applicable Automation groups.
A shared E2E run does not replace an external Task's install, train, and play checks.
A deployment change still needs its scene and policy evidence.

For an affected UE batch:

1. Build the required Editor target from the frozen source.
2. Run a small startup check to detect host failures early.
3. Run the required UE Automation and real E2E cases.
4. Run applicable training, continuation, export, or deployment acceptance procedures.
5. Record commands, versions, completed cases, exit codes, logs, and owned-process shutdown results.

The [test guide](../../tests/README.md) provides runner commands.
Use `scripts/run_all_tests.py` when the agreed scope requires the full repository gate.
Its Python, Automation, and E2E results remain separate evidence layers.
Documentation-only changes do not require UE solely because a batch workflow exists.

A failed or missing required result blocks the batch from `main`.
Identify the failed contract and repair it in the responsible feature or a linked repair PR.
Freeze the revised candidate and repeat affected checks, including shared behavior that the repair can change.
Do not remove required cases, reduce thresholds, or claim that a smoke run proves learning quality.

## Merge the validated content

Before the integration PR merges:

1. Confirm that all included features have completed review and applicable acceptance.
2. Confirm that required checks refer to the final candidate.
3. Compare the proposed merge result with the validated source tree.
4. Confirm that the current `main` base introduces no unvalidated content.
5. Obtain the required merge authorization and use the repository's permitted merge method.

The merge commit ID can differ from the tested commit ID.
The source content entering `main` must match the validated content.
Use Git's existing commit, tree, and diff information to establish that relationship.
Do not replace source verification with branch-name equality.

If `main` advances, update the candidate and reassess the required checks before merging.
If a feature is added, removed, edited, or rebased with conflict resolutions, the candidate has changed.
Repeat affected checks and record why any unchanged evidence still applies.
Do not validate a combined batch and then merge a different subset without that assessment.

Record the final `main` commit and link the batch evidence.
Close only Issues whose acceptance criteria are complete.
Delete completed feature and integration branches when no work still needs them.
Switch the idle validation worktree to the resulting `main` revision before deleting its integration branch.
Preserve uncommitted work and reports when switching revisions.
Retain the worktree, compatible dependencies, and useful caches for the next batch.

## Scope limits

This procedure does not require a permanent `develop` branch.
It does not enable GitHub Merge Queue or change Actions triggers, hooks, or protection rules.
Existing pre-push hooks can still request full UE validation.
Record an unavailable hook check as pending.
Any deferral needs an explicit maintainer decision for that push.
Keep the remaining checks active and complete the required batch checks before `main` integration.

Host permissions, asset availability, and network access remain separate prerequisites.
Batching checks does not resolve those blockers.
The [release workflow](../contribution-workflow.md#7-release-from-verified-code) still applies to a release.

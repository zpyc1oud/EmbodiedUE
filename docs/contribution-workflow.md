# Contribution workflow

Use this workflow for EmbodiedUE changes, including AI-assisted contributions.
[CONTRIBUTING](../CONTRIBUTING.md) defines setup, contribution rights, and documentation conventions.
This page defines the path from proposal to review, merge, and release.

The [roadmap](roadmap/next-release.md) defines release scope.
Accepted [RFCs](rfcs/0001-developer-workflow.md) record design decisions.
Issues track execution.
PRs record implementation and validation evidence.

## 1. Choose the smallest useful proposal

- **Small repair or documentation correction:** open a focused PR. Describe the problem and expected behavior. A separate Issue is optional.
- **Feature or behavior change:** open or use an Issue before substantial implementation. Record use, scope, acceptance behavior, dependencies, and validation. Agree on scope with the maintainer.
- **Architecture or public-contract change:** discuss an RFC before implementation. Record motivation, examples, alternatives, migration, and validation.

Public-contract changes include Task APIs, Run restoration, protocols, artifacts, and training/deployment semantics.
Use RFC 0001 as an example.
If an accepted decision changes, update its RFC.

Search existing Issues and PRs first.
Keep one coherent outcome per PR.
Separate independent work and state dependencies between related PRs.
Use a closing keyword only when the PR completes the referenced Issue's acceptance scope.
Otherwise, use a normal reference and record remaining work.

## 2. Work on a short-lived branch

Use `main` for accepted code.
Create a descriptively named feature branch from the current base, or use a fork when access requires one.
Target `main` for an independent change.
For shared UE validation, use a short-lived `integration/<batch>` branch and a persistent validation worktree.
Selected feature PRs target that integration branch; its batch PR targets `main`.
Follow [batch validation](how-to/batch-validation.md) for preparation, evidence, and cleanup.
A permanent `develop` branch or a release branch requires a separate maintenance decision.

Keep commits understandable and changes reviewable.
Keep a working path through the Python/UE runtime.
Separate unrelated refactors from a repair.
Record the tested revision before branch updates.
After conflict resolution or code changes, repeat the affected checks.
Coordinate shared-branch changes and keep other contributors' uncommitted work.

## 3. Open a Draft, then request review

Open new PRs as Draft while implementation or required evidence is incomplete.
Use the existing PR template when one is available.
Include:

- Problem, final behavior, and related Issue or RFC
- Scope and user-visible changes, including compatibility or migration effects
- Validation commands, results, and evidence links
- Missing host or content requirements
- Remaining blockers and the person responsible for the missing result

Without Windows/UE, an author can supply documentation, Python changes, mocks, and static checks.
Supply available results and request the applicable UE gate from the maintainer.
A mock result does not establish real Chaos behavior.
Draft feedback can start early.
Required runtime evidence must exist before merge into `main`.
A reviewed feature can enter a declared integration batch after its applicable early checks pass.
Its pending UE acceptance must remain explicit in the batch PR.

Mark a PR ready when its implementation and the evidence required for its target branch are ready for inspection.
Reviewers examine behavior, contracts, regression tests, documentation, and actual results.
Separate blocking defects from optional improvements.
Put unrelated suggestions in separate Issues.
Resolve each blocking finding with a repair, evidence, or an agreed scope decision.
After revisions, repeat affected checks rather than use earlier approval as a test result.

## 4. Select validation by the changed behavior

The [test guide](../tests/README.md) defines commands and suite layout.
[Write tests](how-to/write-tests.md) gives concrete implementation examples and assertion rules.
Select applicable rows below.
These are risk boundaries, not a requirement to run every layer for each PR.
Identify the failure that each selected check can detect.

| Change | Evidence expected before merge into `main` |
|---|---|
| Documentation only | Relative links/anchors and described commands, flags, Task IDs, and defaults checked against their authoritative source; no training run solely for prose edits |
| Python logic, configuration, CLI, packaging | Relevant unit/integration/tooling tests and Ruff/Mypy checks; default Python suite for code changes; installed-package checks when packaging or resource lookup changes |
| Wire protocol, plans, observation/action semantics, artifact format | Relevant Python/protocol tests, independent expected values and Python/C++ parity; Editor build and affected native/E2E cases; operator changes follow [operator admission](../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md) |
| UE C++, physics, lifecycle, terrain, Robot assets | Windows Editor build, affected UE Automation and real E2E behavior, including relevant reset/isolation and process-ownership regressions |
| Training, continuation, evaluation, export, deployment | A real affected run-to-game path; separate smoke, numerical parity, physical response, and learning-quality evidence; use the roadmap and theme Issues for release acceptance |
| Fab/other external-content integration | The relevant core checks plus a separately identified authorized-content integration result using the actual content |

From the checkout root, use the applicable commands after setup:

```powershell
uv sync --locked
uv run ruff check src scripts tests
uv run mypy src tests
uv run pytest -q
uv run python scripts/check_docs.py

# Built Windows/UE host required:
uv run python scripts/run_all_tests.py
uv run python scripts/run_e2e.py --suite all
```

Ruff and Mypy use [pyproject.toml](../pyproject.toml) and the repository [hooks](../.pre-commit-config.yaml).
The full runner includes Python, native, and E2E work.
Use the separate E2E entry point for diagnosis or selected reruns.
An unchanged successful run does not require automatic repetition.
Run basic feature checks during development.
Use a frozen integration batch to share expensive UE checks across reviewed features.
The batch must retain each feature's required behavior and scene evidence.

Use the test guide for narrower selections and UE path configuration.
The [setup guide](../README.md#getting-started) defines the Editor build and prerequisites: Windows x64, UE 5.8, Python 3.11, Git LFS, and pinned dependencies.
For GPU validation, record the actual GPU and driver.
CPU policy execution still requires UE simulation.

### Evidence must describe what actually ran

For each invocation, keep:

- Exact commit and any dirty diff
- Command and applicable host/tool versions
- Selected and completed test names
- Result and accessible log or report location

Keep outputs from different invocations separate.
Use these result states:

- **Passed:** execution completed and all selected behavior assertions passed.
- **Failed:** execution or an assertion failed. Keep the actionable failure.
- **Skipped:** the case was deliberately omitted. State the reason and missing scope.
- **Blocked:** a required dependency, host, content, or completion result is unavailable.

A required skipped, blocked, missing, or zero-test result cannot support a full-pass claim.
Historical logs apply to their recorded revision.
For a later revision, identify changes and repeat affected checks.
Do not relabel old evidence without that assessment.

Messages such as `[VERIFY]` are observations, not assertions.
Assert independently observable effects.
Where isolation matters, include unaffected Slots.
Expected results must not repeat the implementation under examination.

For a regression repair, show that the test detects the defect where practical.
Do not remove assertions, reduce thresholds, omit required cases, or replace a required scene solely for a pass.
A legitimate contract or tolerance change requires reasons, review, and updated evidence.
A one-iteration smoke run does not establish learning quality.
Numerical parity does not establish complete scene behavior.

### Current test-gate work

[Issue #13](https://github.com/zpyc1oud/EmbodiedUE/issues/13) has the closed reason `not_planned`.
Its proposed suite-selection and completion work is not the active plan.
[Issue #19](https://github.com/zpyc1oud/EmbodiedUE/issues/19) completed the layered audit and Windows follow-up verification on 2026-10-07.
[Issue #29](https://github.com/zpyc1oud/EmbodiedUE/issues/29) tracks the remaining repository-wide test-quality review.
Examine selected coverage and completion evidence.
Aggregate success alone does not prove that every required case ran.

PRs #9–#12 are historical merged work.
Their integration-specific gates are no longer pending.
This history does not establish acceptance of new changes.
Record core behavior and authorized-content evidence separately.
If a full or release scope requires Pursuit but its content or integration is absent, that scope is blocked.
Prior conditional authorization remains limited to its original actions and conditions.
This workflow does not extend that authorization or add unrelated acceptance work.

## 5. Handle content and host constraints explicitly

Retrieve authorized `.uasset` and `.umap` files through Git LFS.
A pointer is not a usable asset.
Use [third-party notices](../THIRD_PARTY_NOTICES.md) and the [publication inventory](release-readiness.md) for rights and distribution decisions.
Keep externally acquired Fab source assets out of contributions.
Each contributor must acquire the required content under applicable rights.
See [optional Egypt setup](how-to/optional-egypt-demo.md).

Report core and authorized-content integration results separately.
If a required scene is unavailable, report the selected scope as blocked.
A different map or silent skip does not meet the original requirement.
Keep evidence from the real authorized integration.
Share redacted evidence and only artifacts with the necessary distribution rights.
Agree on a location before large uploads.

## 6. Merge with a recorded decision

The author owns scope, implementation, regression evidence, and responses to review.
Reviewers identify concrete defects and assess whether evidence supports the claim.
The maintainer owns acceptance, arranges missing supported-host checks, and performs or authorizes merge.
Write access and automation assistance do not replace that decision.

Before merge into `main`:

1. Examine the final diff and applicable checks.
2. Resolve blocking findings.
3. Make sure that the PR targets the intended base.
4. For a batch, compare the proposed merge result with the validated source tree.
5. Select a merge method enabled by the repository.

Keep each change easy to trace and revert.
For dependent PRs, use the documented integration order.
Validate the combined revision where behaviors can interact.
Record the resulting commit and evidence.
Delete a completed branch when no work needs it.
Close only Issues with complete acceptance evidence.

At baseline `44967ea` on 2026-10-05, the repository had local hooks but no checked-in `.github` workflows or templates.
The visible `Default` ruleset was disabled.
Since then, `.github/workflows/static-checks.yml` was added.
It runs Ruff and Mypy on PRs to `main` and pushes to `main`.

This workflow does not validate documentation links or configuration files.
It does not run Python tests, UE, Chaos, or GPU checks.
A green status proves static checks only.
Required statuses and repository controls remain separate maintainer decisions.
The local documentation hook and default Python suite run `scripts/check_docs.py`.
Use [the documentation check](../CONTRIBUTING.md#check-documentation) for its scope and limitations.

AI assistants use this workflow within the user's authorization.
Preparation, review, or implementation requests alone do not authorize merge, release publication, deployment, access changes, or history changes.
Record each specific conditional approval.
Ask before an action outside that approval.

## 7. Release from verified code

The [roadmap](roadmap/next-release.md) and [release-readiness inventory](release-readiness.md) define the release decision.
Feature merge, release readiness, and public distribution are separate decisions.

1. Decide the compatibility promise, then select the version.
   Keep package, plugin, tag, and release notes consistent.
   The next version and date remain unassigned.
   Explain breaking changes even with Early-Stage status.
2. Freeze the candidate revision, supported dependency versions, migration guidance, and acceptance conditions.
   Include Windows, UE, and Python versions.
   Link independent Windows and behavior evidence required by the roadmap.
   Record missing coverage.
3. Explain changes to Task APIs, CLI/configuration, Run/checkpoint semantics, and policy artifacts.
   State which earlier inputs still work, require recovery or re-export, require new training, or fail validation.
   Keep original Runs and state.
   Avoid unsupported compatibility layers.
4. Prepare release notes with change links, supported configurations, migration steps, evidence, and known limitations.
   Examine generated release notes before publication.
   Make sure that Git and LFS content has the necessary provenance and distribution rights.
5. After authorization, tag the verified commit and publish the intended release or prerelease.
   Make sure that the tag and assets match the tested candidate.

After a regression, prefer a focused corrective PR or reviewed revert.
For a faulty release, identify its version and the usable earlier revision.
Explain Run and artifact compatibility limits.
Publish an authorized correction without silently moving a released tag.
Rollback instructions must protect existing Runs and state.
State whether rollback needs matching configuration, checkpoint, plugin, or artifact versions.

## GitHub references

This workflow adapts GitHub's lightweight contribution tools to EmbodiedUE's
Python/Windows/UE and licensed-content validation needs:

- [Contribution guidelines](https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/setting-guidelines-for-repository-contributors)
- [GitHub flow](https://docs.github.com/en/get-started/using-github/github-flow)
- [Issue and PR templates](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/about-issue-and-pull-request-templates)
- [Reviewing pull requests](https://docs.github.com/en/pull-requests/how-tos/review-pull-requests)
- [Rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/about-rulesets)
- [Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases) and [generated release notes](https://docs.github.com/en/repositories/releasing-projects-on-github/automatically-generated-release-notes)

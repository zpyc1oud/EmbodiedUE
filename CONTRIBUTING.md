# Contributing

[Project home](README.md)

## Contents

- [Contributing](#contributing)
- [Contribution workflow](#contribution-workflow)
- [Validate a feature batch](#batch-validation)


<a id="contributing"></a>

<a id="contributing-contributing"></a>
## Contributing

EmbodiedUE helps game developers train and deploy physics-driven robots in Unreal Engine.
It contains Python training code, a UE 5.8 C++ plugin, and binary content.
Read the [product direction](README.md#from-training-to-gameplay) before proposing a new feature.
Start with [setup](README.md#getting-started), [architecture](docs/architecture.md#architecture), and the [domain glossary](docs/architecture.md#context).

Original project code, documentation, and configuration use [Apache-2.0](LICENSE).
Contributions intentionally submitted for inclusion are covered by that license,
unless explicitly stated otherwise and agreed with the maintainer. Submit only work
you have the right to contribute; retain third-party notices and identify any separate
licenses. Asset contributions need their own provenance and redistribution permission.
See [third-party notices](THIRD_PARTY_NOTICES.md) and [release readiness](docs/release-readiness.md).

The project is **Early-Stage**.
Interfaces and procedures can change without a backward-compatibility guarantee.

<a id="contributing-propose-a-focused-change"></a>
### Propose a focused change

Use the [contribution workflow](CONTRIBUTING.md#contribution-workflow) for proposals, branches, PRs, validation, review, merge, and release decisions.

Describe the problem, expected behavior, and reproduction procedure.
For a bug, include the Task, commit, versions, command, and redacted logs.
For a design change, explain the training or deployment contract and a representative use case.

Keep the repair within the current product scope.
For a new robot, use assets and Python declarations with the existing runtime.
For plan operators, obey the [operator admission requirements](docs/architecture.md#operators).
These include Python and C++ implementations, reviewed parity data, and a negative self-check.

<a id="contributing-develop-locally"></a>
### Develop locally

Run these commands in your development checkout:

```powershell
uv sync --locked
uv run pytest -q
```

Use [Write tests](tests/README.md#write-tests) to select a layer and write the assertions.
Then complete the applicable checks in [tests/README.md](tests/README.md#test-suites).
Python tests do not establish correct Chaos behavior.
For UE changes, build the Editor and run the applicable Automation and E2E cases on Windows.
Record each unavailable check.
For several reviewed features, use [batch validation](CONTRIBUTING.md#batch-validation).
Keep basic checks in feature development and share expensive UE checks on a frozen integration candidate.
Keep the validation worktree and compatible build caches between batches.

The repository has pre-commit and pre-push hooks for Ruff, Mypy, and tests.
Full pre-push checks can require UE.
Report an unavailable runtime check as blocked.

<a id="contributing-documentation-and-examples"></a>
### Documentation and examples

Write the main documentation in English.
Keep `README.zh-CN.md` aligned with the English README when first-use steps or product claims change.
Both READMEs must use the same commands, Task IDs, defaults, and support scope.
Use short sentences and consistent project terms.
Put prerequisites before instructions.
Put expected results next to the related action.

Use repository-relative links and reproducible commands.
Use placeholders for host-specific paths.
Keep first-use instructions in the README, operating procedures in `docs/training.md` and `docs/deployment.md`, and shared contracts in `docs/architecture.md`.
Keep test authoring and execution in `tests/README.md`.
Record development plans, audits, and experiment progress in Issues, PRs, or external artifacts, not new repository documents.
Keep experiment reports and development journals out of public product guides.
Measurements from one host do not establish compatibility or performance guarantees.

For documentation changes, do these checks:

- Make sure that relative links and anchors resolve.
- Compare commands and flags with CLI help or their parser definitions.
- Compare Task IDs with `uerl tasks` or the registry.
- Compare defaults with resolved configurations.

Do not start training solely for a documentation change.

<a id="contributing-organize-documentation-for-the-reader"></a>
#### Organize documentation for the reader

Keep four kinds of content easy to find:

- The README explains the product and gives a small first-use path.
- Procedure guides state prerequisites, actions, expected results, and the next step.
- Technical references define configuration, interfaces, and ownership.
- Design records state their status and link to implementation work.

Keep the game developer's workflow visible in the documentation index.
Use existing guides for details instead of repeating long procedures on several pages.
A recording illustrates behavior; measurements establish the result for a specified setup.

The following official projects provide useful documentation patterns:

| Reference | Pattern used here |
|---|---|
| [Isaac Lab README](https://github.com/isaac-sim/IsaacLab) | A clear product introduction with separate installation, learning, and task entry points |
| [Unity ML-Agents getting started](https://unity-technologies.github.io/ml-agents/Getting-Started/) | A complete example that connects training to a model running in a game environment |
| [MuJoCo documentation](https://mujoco.readthedocs.io/en/stable/overview.html) | Separate overview, modeling, programming, and technical reference sections |
| [Godot documentation introduction](https://docs.godotengine.org/en/stable/about/introduction.html#organization-of-the-documentation) | Distinct paths for new users, feature lookup, and engine contributors |

Use these structural ideas with EmbodiedUE's supported commands and behavior.
Write project-specific explanations rather than copy another project's prose or capability claims.

<a id="contributing-review-evidence"></a>
### Review evidence

Describe the problem, final behavior, completed validation, and remaining limitations.
For an experiment, keep the configuration, seed, commit, dirty diff, manifest, checkpoint identity, and evaluation procedure together.
Generated `runs/` output is not source content.
Agree on a location before you share large artifacts.

Keep credentials, private host paths, confidential logs, and unauthorized assets out of contributions.
Git LFS applies to `.uasset` and `.umap` files.
Before you add binary content, obtain the applicable redistribution permission.

<a id="contributing-check-documentation"></a>
### Check documentation

Run the project documentation check after you change prose, links, or CLI arguments:

```powershell
uv run python scripts/check_docs.py
```

The pre-commit hook runs the same command for Markdown and CLI changes.
The default Python suite also checks the project documents.
The command selects tracked project Markdown files and excludes imported `.agents/skills` documents.
Stage a new Markdown file before you run this check.

The check finds missing local link targets, missing heading anchors, and unclosed code fences.
It supports inline and reference links, encoded paths, and duplicate headings.
It parses fenced `uerl` and `uv run uerl` examples with the actual CLI argument parsers.
It joins PowerShell and POSIX continuation lines.
Shell variables and explicit placeholders use representative typed values where needed.
The check does not execute the examples or start UE.

Review runtime semantics, Task IDs, defaults, shell expressions, and external URLs separately.
The check does not validate those items or generated README templates.
GitHub Actions retains its Ruff/Mypy scope.

<a id="contribution-workflow"></a>

<a id="contribution-workflow-contribution-workflow"></a>
## Contribution workflow

Use this workflow for EmbodiedUE changes, including AI-assisted contributions.
[CONTRIBUTING](CONTRIBUTING.md#contributing) defines setup, contribution rights, and documentation conventions.
This page defines the path from proposal to review, merge, and release.

The [roadmap](https://github.com/zpyc1oud/EmbodiedUE/issues/51) defines release scope.
Accepted [RFCs](docs/architecture.md#architecture-architecture-and-design-rationale) record design decisions.
Issues track execution.
PRs record implementation and validation evidence.

<a id="contribution-workflow-1-choose-the-smallest-useful-proposal"></a>
### 1. Choose the smallest useful proposal

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

<a id="contribution-workflow-2-work-on-a-short-lived-branch"></a>
### 2. Work on a short-lived branch

Use `main` for accepted code.
Create a descriptively named feature branch from the current base, or use a fork when access requires one.
Target `main` for an independent change.
For shared UE validation, use a short-lived `integration/<batch>` branch and a persistent validation worktree.
Selected feature PRs target that integration branch; its batch PR targets `main`.
Follow [batch validation](CONTRIBUTING.md#batch-validation) for preparation, evidence, and cleanup.
A permanent `develop` branch or a release branch requires a separate maintenance decision.

Keep commits understandable and changes reviewable.
Keep a working path through the Python/UE runtime.
Separate unrelated refactors from a repair.
Record the tested revision before branch updates.
After conflict resolution or code changes, repeat the affected checks.
Coordinate shared-branch changes and keep other contributors' uncommitted work.

<a id="contribution-workflow-3-open-a-draft-then-request-review"></a>
### 3. Open a Draft, then request review

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

<a id="contribution-workflow-4-select-validation-by-the-changed-behavior"></a>
### 4. Select validation by the changed behavior

The [test guide](tests/README.md#test-suites) defines commands and suite layout.
[Write tests](tests/README.md#write-tests) gives concrete implementation examples and assertion rules.
Select applicable rows below.
These are risk boundaries, not a requirement to run every layer for each PR.
Identify the failure that each selected check can detect.

| Change | Evidence expected before merge into `main` |
|---|---|
| Documentation only | Relative links/anchors and described commands, flags, Task IDs, and defaults checked against their authoritative source; no training run solely for prose edits |
| Python logic, configuration, CLI, packaging | Relevant unit/integration/tooling tests and Ruff/Mypy checks; default Python suite for code changes; installed-package checks when packaging or resource lookup changes |
| Wire protocol, plans, observation/action semantics, artifact format | Relevant Python/protocol tests, independent expected values and Python/C++ parity; Editor build and affected native/E2E cases; operator changes follow [operator admission](docs/architecture.md#operators) |
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

Ruff and Mypy use [pyproject.toml](pyproject.toml) and the repository [hooks](.pre-commit-config.yaml).
The full runner includes Python, native, and E2E work.
Use the separate E2E entry point for diagnosis or selected reruns.
An unchanged successful run does not require automatic repetition.
Run basic feature checks during development.
Use a frozen integration batch to share expensive UE checks across reviewed features.
The batch must retain each feature's required behavior and scene evidence.

Use the test guide for narrower selections and UE path configuration.
The [setup guide](README.md#getting-started) defines the Editor build and prerequisites: Windows x64, UE 5.8, Python 3.11, Git LFS, and pinned dependencies.
For GPU validation, record the actual GPU and driver.
CPU policy execution still requires UE simulation.

<a id="contribution-workflow-evidence-must-describe-what-actually-ran"></a>
#### Evidence must describe what actually ran

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

<a id="contribution-workflow-5-handle-content-and-host-constraints-explicitly"></a>
### 5. Handle content and host constraints explicitly

Retrieve authorized `.uasset` and `.umap` files through Git LFS.
A pointer is not a usable asset.
Use [third-party notices](THIRD_PARTY_NOTICES.md) and the [publication inventory](docs/release-readiness.md) for rights and distribution decisions.
Keep externally acquired Fab source assets out of contributions.
Each contributor must acquire the required content under applicable rights.
See [optional Egypt setup](docs/deployment.md#optional-egypt-demo).

Report core and authorized-content integration results separately.
If a required scene is unavailable, report the selected scope as blocked.
A different map or silent skip does not meet the original requirement.
Keep evidence from the real authorized integration.
Share redacted evidence and only artifacts with the necessary distribution rights.
Agree on a location before large uploads.

<a id="contribution-workflow-6-merge-with-a-recorded-decision"></a>
### 6. Merge with a recorded decision

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
Use [the documentation check](CONTRIBUTING.md#contributing-check-documentation) for its scope and limitations.

AI assistants use this workflow within the user's authorization.
Preparation, review, or implementation requests alone do not authorize merge, release publication, deployment, access changes, or history changes.
Record each specific conditional approval.
Ask before an action outside that approval.

<a id="contribution-workflow-7-release-from-verified-code"></a>
### 7. Release from verified code

The [roadmap](https://github.com/zpyc1oud/EmbodiedUE/issues/51) and [release-readiness inventory](docs/release-readiness.md) define the release decision.
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

<a id="contribution-workflow-github-references"></a>
### GitHub references

This workflow adapts GitHub's lightweight contribution tools to EmbodiedUE's
Python/Windows/UE and licensed-content validation needs:

- [Contribution guidelines](https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/setting-guidelines-for-repository-contributors)
- [GitHub flow](https://docs.github.com/en/get-started/using-github/github-flow)
- [Issue and PR templates](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/about-issue-and-pull-request-templates)
- [Reviewing pull requests](https://docs.github.com/en/pull-requests/how-tos/review-pull-requests)
- [Rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/about-rulesets)
- [Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases) and [generated release notes](https://docs.github.com/en/repositories/releasing-projects-on-github/automatically-generated-release-notes)

<a id="batch-validation"></a>

<a id="batch-validation-validate-a-feature-batch"></a>
## Validate a feature batch

Use a persistent Windows worktree to validate several reviewed features together.
Keep the worktree, dependencies, and useful build caches between batches.
Use a new, short-lived integration branch for each batch.

The [contribution workflow](CONTRIBUTING.md#contribution-workflow) defines review and merge requirements.
This procedure changes when shared UE checks run, not the required evidence.
A small change can still use a direct PR to `main`.

<a id="batch-validation-prepare-the-validation-worktree"></a>
### Prepare the validation worktree

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

<a id="batch-validation-review-features-before-integration"></a>
### Review features before integration

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

<a id="batch-validation-assemble-one-candidate"></a>
### Assemble one candidate

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

<a id="batch-validation-freeze-and-validate-the-batch"></a>
### Freeze and validate the batch

Freeze the integration head before expensive host checks.
Record its commit, the `main` base, and any dirty diff.
If UE generates local configuration changes, keep that diff with the report.
Explain whether those changes are host-only or require a reviewed source change.

Use the [validation matrix](CONTRIBUTING.md#contribution-workflow-4-select-validation-by-the-changed-behavior) to select the batch's required checks.
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

The [test guide](tests/README.md#test-suites) provides runner commands.
Use `scripts/run_all_tests.py` when the agreed scope requires the full repository gate.
Its Python, Automation, and E2E results remain separate evidence layers.
Documentation-only changes do not require UE solely because a batch workflow exists.

A failed or missing required result blocks the batch from `main`.
Identify the failed contract and repair it in the responsible feature or a linked repair PR.
Freeze the revised candidate and repeat affected checks, including shared behavior that the repair can change.
Do not remove required cases, reduce thresholds, or claim that a smoke run proves learning quality.

<a id="batch-validation-merge-the-validated-content"></a>
### Merge the validated content

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

<a id="batch-validation-scope-limits"></a>
### Scope limits

This procedure does not require a permanent `develop` branch.
It does not enable GitHub Merge Queue or change Actions triggers, hooks, or protection rules.
Existing pre-push hooks can still request full UE validation.
Record an unavailable hook check as pending.
Any deferral needs an explicit maintainer decision for that push.
Keep the remaining checks active and complete the required batch checks before `main` integration.

Host permissions, asset availability, and network access remain separate prerequisites.
Batching checks does not resolve those blockers.
The [release workflow](CONTRIBUTING.md#contribution-workflow-7-release-from-verified-code) still applies to a release.

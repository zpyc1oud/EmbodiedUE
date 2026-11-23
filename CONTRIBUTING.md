# Contributing

[Project home](README.md)

## Contents

- [Contributing](#contributing)
- [Contribution workflow](#contribution-workflow)
- [Validate a feature batch](#batch-validation)

<a id="contributing"></a>

<a id="contributing-contributing"></a>
## Contributing

EmbodiedUE helps game developers train and deploy physics-driven robots in Unreal Engine 5.8 using Python training code, a UE C++ plugin, and binary content.
Before proposing changes, read the [product direction](README.md#from-training-to-gameplay), [setup](README.md#getting-started), [architecture](docs/architecture.md#architecture), and [domain glossary](docs/architecture.md#context).

Original code, documentation, and configuration are licensed under [Apache-2.0](LICENSE).
Contributions intentionally submitted for inclusion are covered by that license unless explicitly stated otherwise and agreed with the maintainer.
Submit only work you have the right to contribute, preserve third-party notices, and obtain redistribution permission before adding binary assets (`.uasset`/`.umap` tracked by Git LFS).
See [third-party notices](THIRD_PARTY_NOTICES.md) and [release readiness](docs/release-readiness.md).

The project is **Early-Stage**: interfaces and procedures may change without backward-compatibility guarantees.

<a id="contributing-propose-a-focused-change"></a>
### Propose a focused change

Follow the [contribution workflow](CONTRIBUTING.md#contribution-workflow) for proposals, branches, PRs, validation, review, merge, and release decisions.

- State the problem, expected behavior, and reproduction steps (include Task ID, commit, versions, command, and redacted logs for bugs; training/deployment contract and use case for design changes).
- Keep changes within product scope: add new robots via UE assets and Python declarations rather than robot-specific UE runtime C++.
- New plan operators must satisfy the [operator admission requirements](docs/architecture.md#operators) (Python and C++ implementations, reviewed parity data, and a negative self-check).

<a id="contributing-develop-locally"></a>
### Develop locally

```powershell
uv sync --locked
uv run pytest -q
```

Use [Write tests](tests/README.md#write-tests) and the [test suites](tests/README.md#test-suites) to choose assertions and run applicable checks.
Python mocks do not validate Chaos physics: UE changes require building the Editor on Windows and running the applicable Automation and E2E suites.
If several reviewed features share expensive UE checks, follow [batch validation](CONTRIBUTING.md#batch-validation).
Local pre-commit/pre-push hooks enforce Ruff, Mypy, and tests; report any unavailable runtime check as blocked rather than passed.

<a id="contributing-documentation-and-examples"></a>
### Documentation and examples

- Write primary docs in English and keep `README.zh-CN.md` aligned on commands, Task IDs, defaults, and support claims.
- Use short sentences, consistent terms, repo-relative links, reproducible commands, and host-path placeholders.
- Place setup in READMEs, daily training/video workflows in `docs/training.md`, troubleshooting/diagnostics in `docs/troubleshooting.md`, extension guides in `docs/extending.md`, deployment in `docs/deployment.md`, contracts in `docs/architecture.md`, and test instructions in `tests/README.md`.
- Track development plans and experiment progress in GitHub Issues (link PRs and supporting artifacts from the Issue), not in new product docs.
- Verify links, anchors, CLI flags, Task IDs (`uerl tasks`), and defaults (`uerl config`); do not start training solely for documentation edits.

<a id="contributing-organize-documentation-for-the-reader"></a>
#### Organize documentation for the reader

Follow four distinct roles—overviews (`README.md`), task procedures (`docs/training.md`, `docs/deployment.md`), technical references (`docs/architecture.md`, `docs/extending.md`), and design records (RFCs)—keeping the game developer's end-to-end workflow prominent and linking instead of duplicating procedures.

<a id="contributing-review-evidence"></a>
### Review evidence

Summarize the problem, final behavior, completed checks, and remaining limitations.
For experiments, preserve configuration, seed, commit, dirty diff, manifest, checkpoint ID, and evaluation steps; do not commit generated `runs/` outputs, credentials, private host paths, or unauthorized third-party assets.

<a id="contributing-check-documentation"></a>
### Check documentation

Run the documentation checker after changing prose, links, or CLI examples:

```powershell
uv run python scripts/check_docs.py
```

This checks tracked Markdown files (excluding `.agents/skills`, so stage new Markdown files first) for broken local paths, missing heading anchors, unclosed code fences, and CLI example parsing against `uerl` argument parsers.
It does not execute commands, verify Task IDs/defaults/external URLs, or start UE.

<a id="contribution-workflow"></a>

<a id="contribution-workflow-contribution-workflow"></a>
## Contribution workflow

This workflow governs human and AI-assisted changes from proposal through release.
The [roadmap](https://github.com/zpyc1oud/EmbodiedUE/issues/51) sets release scope, accepted [RFCs](docs/architecture.md#architecture-architecture-and-design-rationale) record architectural decisions, GitHub Issues hold the scope, acceptance criteria, and progress for each change, and PRs record implementation and validation evidence.

<a id="contribution-workflow-1-choose-the-smallest-useful-proposal"></a>
### 1. Choose the smallest useful proposal

1. Search [GitHub Issues](https://github.com/zpyc1oud/EmbodiedUE/issues) and PRs, and reuse or open a focused Issue before implementation (for fixes, docs, features, and training experiments).
2. Record the problem, intended outcome, acceptance evidence, dependencies, and ongoing progress in that Issue.
3. For public-contract or architecture changes (Task APIs, Run restoration, protocols, artifacts, or training/deployment semantics), link and discuss an RFC using RFC 0001 as an example.

Keep one coherent outcome per PR, state cross-PR dependencies, and use closing keywords only when the referenced Issue's full acceptance scope is complete.

<a id="contribution-workflow-2-work-on-a-short-lived-branch"></a>
### 2. Work on a short-lived branch

Target `main` on a short-lived feature branch (or fork).
For shared UE validation across reviewed features, target an `integration/<batch>` branch per [batch validation](CONTRIBUTING.md#batch-validation).
Keep commits clear, separate unrelated refactors from bug fixes, and re-run affected checks after conflict resolution or revisions.

<a id="contribution-workflow-3-open-a-draft-then-request-review"></a>
### 3. Open a Draft, then request review

Open PRs as Draft while code or required evidence is incomplete, filling out the PR template with problem/final behavior, the GitHub Issue (plus RFC when applicable), scope/compatibility impact, exact validation commands/results, missing host/content requirements, and remaining blockers.
Without Windows/UE, supply Python/doc checks and request maintainer UE verification; required runtime evidence must land before merging into `main`.
When marked Ready for review, address all blocking findings with fixes, evidence, or agreed scope adjustments, and repeat affected checks after changes.

<a id="contribution-workflow-4-select-validation-by-the-changed-behavior"></a>
### 4. Select validation by the changed behavior

Select rows matching your risk boundary (see [tests/README.md](tests/README.md#test-suites) and [Write tests](tests/README.md#write-tests)):

| Change | Evidence expected before merge into `main` |
|---|---|
| Documentation only | Links, anchors, flags, Task IDs, and defaults checked; no training run needed |
| Python logic, configuration, CLI, packaging | Targeted unit/integration/tooling tests, Ruff/Mypy, default `pytest` suite, and isolated-package checks if packaging/resources changed |
| Wire protocol, plans, observation/action semantics, artifact format | Protocol tests, independent numerical parity, Editor build, and affected native/E2E tests; follow [operator admission](docs/architecture.md#operators) |
| UE C++, physics, lifecycle, terrain, Robot assets | Windows Editor build, affected UE Automation tests, real E2E behavior, and reset/slot-isolation/process-ownership checks |
| Training, continuation, evaluation, export, deployment | Real affected run-to-game path keeping smoke, numerical parity, physical response, and learning-quality evidence distinct |
| Fab/other external-content integration | Core checks plus a separately labeled authorized-content run |

Standard verification commands from the checkout root:

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

`pyproject.toml` configures Ruff/Mypy and `.pre-commit-config.yaml` defines local hooks; see [setup](README.md#getting-started) for host prerequisites and record GPU/driver details for GPU runs.

<a id="contribution-workflow-evidence-must-describe-what-actually-ran"></a>
#### Evidence must describe what actually ran

Record exact commit, dirty diff, commands, versions, executed test names, result state (**Passed**, **Failed**, **Skipped**, or **Blocked**), and logs for each run.

- A skipped, blocked, missing, or zero-test run cannot support a full-pass claim, nor can un-reassessed logs from an older commit.
- Diagnostic `[VERIFY]` messages are observations, not assertions; assert independent expectations and include unaffected Slots when testing isolation.
- Never remove assertions, relax tolerances, omit cases, or swap required scenes to obtain a pass; one-iteration smoke runs do not establish learning quality.

<a id="contribution-workflow-5-handle-content-and-host-constraints-explicitly"></a>
### 5. Handle content and host constraints explicitly

Pull authorized `.uasset`/`.umap` files with Git LFS, consult [third-party notices](THIRD_PARTY_NOTICES.md) and [release readiness](docs/release-readiness.md), and never commit externally acquired Fab source content (see [optional Egypt setup](docs/deployment.md#optional-egypt-demo)).
Report missing scenes as **Blocked** rather than silently substituting another map or skipping checks.

<a id="contribution-workflow-6-merge-with-a-recorded-decision"></a>
### 6. Merge with a recorded decision

The author owns scope, implementation, and regression proof; reviewers verify contracts and evidence; the maintainer owns acceptance and merge authorization.
Before merging into `main`, verify the final diff/checks match the tested tree, resolve blocking findings, confirm the base branch, and use a permitted merge method, then delete completed branches and record the merge commit.

`.github/workflows/static-checks.yml` runs Ruff, Mypy, `scripts/check_docs.py`, and Python `pytest` on PRs and pushes to `main`.
It does not run Unreal Engine C++ builds, Chaos physics, or GPU tests—maintainer/host checks remain required for native/physics/training changes.
AI assistants must stay within explicit user authorization for merges, releases, history rewrites, or credential/access changes.

<a id="contribution-workflow-7-release-from-verified-code"></a>
### 7. Release from verified code

Release readiness follows the [roadmap](https://github.com/zpyc1oud/EmbodiedUE/issues/51) and [release-readiness inventory](docs/release-readiness.md):

1. Align the version across Python packages, `.uplugin` files, examples, tags, and release notes; explain breaking changes cleanly even at Early-Stage status.
2. Freeze the candidate commit, supported Windows/UE/Python versions, migration guidance, and Windows runtime evidence (explicitly noting missing coverage).
3. Document Run, Task API, CLI/config, checkpoint, and `.uerlpol2` artifact compatibility without adding unsupported compatibility layers.
4. Publish release notes and tag only after verifying provenance/distribution rights for all Git and LFS assets; never silently move a released tag (use corrective releases instead, with rollback guidance protecting existing Runs).

<a id="contribution-workflow-github-references"></a>
### GitHub references

- [Contribution guidelines](https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/setting-guidelines-for-repository-contributors)
- [GitHub flow](https://docs.github.com/en/get-started/using-github/github-flow)
- [Issue and PR templates](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/about-issue-and-pull-request-templates)
- [Reviewing pull requests](https://docs.github.com/en/pull-requests/how-tos/review-pull-requests)
- [Rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/about-rulesets)
- [Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases) and [generated release notes](https://docs.github.com/en/repositories/releasing-projects-on-github/automatically-generated-release-notes)

<a id="batch-validation"></a>

<a id="batch-validation-validate-a-feature-batch"></a>
## Validate a feature batch

Use a dedicated persistent Windows worktree and short-lived `integration/<batch>` branch when sharing expensive UE build/test runs across multiple reviewed features; small changes can still go directly to `main`.

<a id="batch-validation-prepare-the-validation-worktree"></a>
### Prepare the validation worktree

Maintain a clean, single-operator worktree separate from feature checkouts (e.g., `E:/EmbodiedUE-validation`), keeping prior `runs/` and build caches intact when dependencies and UE versions match and running clean builds only when incremental output is suspect.
Never run concurrent builds or mutate revisions while Unreal workers are active.

<a id="batch-validation-review-features-before-integration"></a>
### Review features before integration

Each feature PR must pass unit/regression tests, Ruff, Mypy, doc checks, and isolated-package checks where applicable before entering an integration branch; list pending UE checks explicitly.

<a id="batch-validation-assemble-one-candidate"></a>
### Assemble one candidate

Create `integration/<batch>` from `main`, open a Draft integration PR targeting `main`, retarget and merge approved feature PRs in dependency order, and re-run early checks after conflict resolution.
Do not close feature Issues solely upon batch assembly; CI checks only run automatically once targeting `main`.

<a id="batch-validation-freeze-and-validate-the-batch"></a>
### Freeze and validate the batch

Freeze the candidate revision and record its commit, base `main` hash, and any UE-generated config diff.
Select required checks from the [validation matrix](CONTRIBUTING.md#contribution-workflow-4-select-validation-by-the-changed-behavior), share single Editor builds across compatible Automation groups while keeping feature-specific E2E/scene/install/deploy evidence intact (or run `scripts/run_all_tests.py` for full gates), and fix any failure in a linked repair before re-freezing.

<a id="batch-validation-merge-the-validated-content"></a>
### Merge the validated content

Before merging into `main`, verify that the merged tree matches the frozen tested source (reassessing checks if `main` or included features moved).
Record the final `main` commit, close only Issues with complete acceptance evidence, switch the validation worktree to updated `main`, and delete merged branches.

<a id="batch-validation-scope-limits"></a>
### Scope limits

Batch validation keeps local pre-push/CI hooks unchanged and does not bypass host, asset, network, or [release workflow](CONTRIBUTING.md#contribution-workflow-7-release-from-verified-code) requirements.

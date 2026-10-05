# Contribution workflow

Use this workflow for changes to EmbodiedUE, including AI-assisted contributions.
[CONTRIBUTING](../CONTRIBUTING.md) owns setup, contribution rights, and writing
conventions. This page owns the path from proposal to review, merge, and release.
The [roadmap](roadmap/next-release.md) owns release scope, accepted
[RFCs](rfcs/0001-developer-workflow.md) own design decisions, Issues track execution,
and PRs record implementation and validation evidence.

## 1. Choose the smallest useful proposal

- **Small fix or documentation correction:** open a focused PR directly. Explain
  the problem and expected behavior; a separate Issue is optional.
- **Feature or behavior change:** open or reuse an Issue before substantial
  implementation. Describe the use case, scope, acceptance behavior, dependencies,
  and intended validation. Agree on scope with the maintainer.
- **Architecture or public-contract change:** discuss an RFC before committing to
  the implementation. Cover motivation, user examples, alternatives, migration,
  and validation. This includes substantial Task, run-restoration, protocol,
  artifact, or training/deployment semantic changes. Use RFC 0001 as the existing
  example; update an accepted RFC if its decision changes.

Search existing Issues and PRs first. Keep one coherent outcome per PR. Split
independent work; make dependencies between related PRs explicit. Link an Issue
with a closing keyword only when the PR completes its acceptance scope. Otherwise
use a normal reference and leave the remaining work visible.

## 2. Work on a short-lived branch

Use `main` as the integration branch. Create a descriptively named branch from the
current base, or work in a fork when repository access requires it. Target `main`
unless a dependent PR explicitly names another base. Avoid a permanent `develop`
branch or speculative release branches; a supported maintenance line needs an
actual release decision.

Keep commits understandable and changes reviewable. Preserve a working path through
the existing Python/UE runtime. Do not mix unrelated refactors into a fix. Record
the tested revision before updating the branch; after conflict resolution or code
changes, rerun the affected checks. Coordinate updates to shared branches and
preserve another contributor's uncommitted work.

## 3. Open a Draft, then request review

Open new PRs as Draft while implementation or required evidence is incomplete.
Include these details in the description, using an existing template if provided:

- Problem, final behavior, and related Issue/RFC.
- Scope and user-visible changes, including compatibility or migration effects.
- Validation commands, results, evidence links, and outstanding host/content needs.
- Any remaining blocker and who will supply the missing result.

An author without Windows/UE can contribute documentation, Python changes, mocks,
and static checks. Provide the checks available locally and ask the maintainer to
run the applicable UE gate. Do not substitute a mock result for real Chaos behavior.
The PR can receive early feedback as a Draft; required runtime evidence still has
to be supplied before merge.

Mark the PR ready when the implementation and applicable evidence are reviewable.
Reviewers check the supported behavior, relevant contracts, regression tests,
documentation, and actual results. Distinguish merge-blocking defects from optional
improvements; move unrelated suggestions to separate Issues. Respond to each
blocking finding with a fix, evidence, or an agreed scope decision. Recheck affected
results after revisions rather than treating an earlier approval as a test result.

## 4. Select validation by the changed behavior

The [test guide](../tests/README.md) is authoritative for commands and suite layout.
Use the rows below that apply to the change; they are risk boundaries, not a demand
to run every layer for every PR. Identify what failure each check could expose.

| Change | Evidence expected before merge |
|---|---|
| Documentation only | Relative links/anchors and described commands, flags, Task IDs, and defaults checked against their authoritative source; no training run solely for prose edits |
| Python logic, configuration, CLI, packaging | Relevant unit/integration/tooling tests and Ruff/Mypy checks; default Python suite for code changes; installed-package checks when packaging or resource lookup changes |
| Wire protocol, plans, observation/action semantics, artifact format | Relevant Python/protocol tests, independent expected values and Python/C++ parity; Editor build and affected native/E2E cases; operator changes follow [operator admission](../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md) |
| UE C++, physics, lifecycle, terrain, Robot assets | Windows Editor build, affected UE Automation and real E2E behavior, including relevant reset/isolation and process-ownership regressions |
| Training, continuation, evaluation, export, deployment | A real affected run-to-game path; separate smoke, numerical parity, physical response, and learning-quality evidence; use the roadmap and theme Issues for release acceptance |
| Fab/other external-content integration | The relevant core checks plus a separately identified authorized-content integration result using the actual content |

Current entry points, run from the checkout root after setup:

```powershell
uv sync --locked
uv run ruff check src scripts tests
uv run mypy src tests
uv run pytest -q

# Built Windows/UE host required:
uv run python scripts/run_all_tests.py
uv run python scripts/run_e2e.py --suite all
```

Ruff and Mypy commands use the tools configured in
[pyproject.toml](../pyproject.toml) and the local
[hooks](../.pre-commit-config.yaml). The full runner includes Python, native, and
E2E work; the separate E2E entry point is useful for targeted diagnosis and reruns,
not a requirement to repeat an unchanged successful run. Consult the test guide
for narrower selections and UE path configuration. The [setup guide](../README.md#getting-started)
owns the Editor build command and supported prerequisites: Windows x64, UE 5.8,
Python 3.11, Git LFS, and the pinned dependencies. Record the actual GPU and driver
for GPU validation. CPU policy execution still requires UE for simulation.

### Evidence must describe what actually ran

For each reported run, retain the exact commit and dirty diff if any, command,
relevant host/tool versions, selected and completed tests, result, and accessible
log/report location. Keep each invocation's outputs separate. Use these statuses:

- **Passed:** the selected behavior ran to completion and its assertions passed.
- **Failed:** execution or assertions failed; retain the actionable failure.
- **Skipped:** a case was deliberately not run; state why and what scope is absent.
- **Blocked:** a required dependency, host, content, or completion result is missing.

A required skipped, blocked, missing, or zero-test result cannot support a full-pass
claim. Old logs apply to their recorded revision. For a later revision, identify
what changed and rerun affected checks; do not silently relabel earlier evidence.
Logs such as `[VERIFY]` are observations, not assertions. Check independently
observable effects and unaffected Slots where isolation matters. Expected results
must not merely repeat the implementation under test.

When fixing a regression, demonstrate that the test detects the fault where
practical. Do not remove assertions, lower thresholds, omit required cases, or
replace the required scene just to obtain a green result. A legitimate change to a
contract or tolerance needs its own rationale, review, and updated evidence.
One-iteration smoke completion does not establish learning quality; numerical
parity does not establish whole-scene behavior.

### Current test-gate work

[Issue #13](https://github.com/zpyc1oud/EmbodiedUE/issues/13) is closed with reason
`not_planned`; its proposed suite-selection and completion work is not the active
test-gate plan. The current layered audit and follow-up verification are tracked in
[Issue #19](https://github.com/zpyc1oud/EmbodiedUE/issues/19). Inspect the selected
runner's coverage and completion evidence rather than assuming an aggregate
success proves every required case ran.

PRs #9–#12 are historical, merged integration work; their integration-specific
gates are no longer pending work. This does not establish acceptance for current
changes: report core behavior and authorized-content evidence separately. If the
selected full or release scope requires Pursuit and its actual content or authored
integration is absent, that scope remains blocked as described below. Existing
conditional authorization remains bounded by its original action and conditions;
this workflow neither expands that authorization nor adds unrelated acceptance
work.

## 5. Handle content and host constraints explicitly

Retrieve authorized `.uasset` and `.umap` files through Git LFS; a pointer file is
not a usable asset. Follow [third-party notices](../THIRD_PARTY_NOTICES.md) and the
[publication inventory](release-readiness.md) for rights and distribution decisions.
Keep externally obtained Fab source assets out of contributions. Contributors must
acquire the required content under their own applicable rights; see the
[optional Egypt setup](how-to/optional-egypt-demo.md).

Report core validation and authorized-content integration separately. If a selected
full/release scope requires Pursuit and its actual content or authored integration
is absent, report that scope as blocked. A different map or a silently skipped case
does not fulfill it. Preserve the result from the real authorized integration when
it is run. Share redacted evidence and only artifacts that can lawfully be shared;
agree on a suitable location before uploading large run outputs.

## 6. Merge with a recorded decision

The author owns scope, implementation, regression evidence, and responses to review.
Reviewers identify concrete defects and judge whether the evidence supports the
claim. The maintainer owns acceptance, arranges missing supported-host checks,
resolves scope decisions, and performs or authorizes the merge. Repository write
access and automation assistance do not replace that decision.

Before merging, confirm the final diff, applicable checks, resolved blocking review
findings, and the PR's intended base. Use a merge method enabled by the repository;
keep a focused change easy to trace and revert. For dependent PRs, integrate in the
documented order and validate the resulting combined revision where behavior can
interact. Record the resulting commit and its evidence. Retire the completed branch
when it is no longer needed, and close only the Issues whose acceptance is met.

This document defines contributor practice. At the 2026-10-05 baseline
(`44967ea`), the tree had local hooks but no checked-in `.github`
workflows/templates, and the visible `Default` ruleset was disabled. Since then,
`.github/workflows/static-checks.yml` has been added. It runs Ruff and Mypy
on pull requests to `main` and pushes to `main`. It does not validate
documentation links or configuration files, or run Python tests, Unreal Engine,
Chaos, or GPU checks. A green result is static-check evidence only, not a runtime
guarantee. Required status checks and other repository controls remain separate
maintainer decisions.

AI assistants follow this workflow within the user's authorized task. Requests to
prepare, review, or implement work do not by themselves authorize merging,
publishing a release, deploying, changing access/settings, or rewriting history.
Record and respect any specific conditional approval; ask when the next action
falls outside it.

## 7. Release from verified code

The [roadmap](roadmap/next-release.md) and
[release-readiness inventory](release-readiness.md) define the release decision.
Feature merge, release readiness, and public distribution are separate decisions.

1. Choose the version after deciding the compatibility promise. Keep package,
   plugin, tag, and release notes consistent; the next version/date remain unassigned
   until that decision. Early-Stage status does not remove the need to explain
   breaking changes.
2. Freeze the candidate revision, supported Windows/UE/Python/dependency versions,
   migration guidance, and acceptance conditions. Link the independent Windows
   setup and behavioral evidence required by the roadmap. Record missing coverage.
3. Explain changes to Task APIs, CLI/configuration, run/checkpoint semantics, and
   policy artifacts. Say which old inputs continue to work, which require explicit
   recovery or re-export/retraining, and which are rejected. Preserve original runs
   and checkpoints; avoid speculative compatibility layers.
4. Prepare release notes with change links, supported configurations, migration
   steps, evidence, and known limitations. Review generated notes if used. Confirm
   provenance and Git/LFS distribution contents before publishing assets or source
   archives, including the historical-content concerns in the publication inventory.
5. After authorization, tag the verified commit and publish the intended release
   or prerelease. Verify the tag/asset contents match the tested candidate.

For a regression after merge, prefer a focused corrective PR or reviewed revert.
For a faulty release, identify the affected version, document the usable previous
revision and any run/artifact compatibility limits, and publish an authorized
correction. Preserve released history rather than silently moving a published tag.
Rollback instructions must protect existing runs and state whether returning to
older code also needs the matching configuration, checkpoint, plugin, or artifact.

## GitHub references

This workflow adapts GitHub's lightweight contribution tools to EmbodiedUE's
Python/Windows/UE and licensed-content validation needs:

- [Contribution guidelines](https://docs.github.com/en/communities/setting-up-your-project-for-healthy-contributions/setting-guidelines-for-repository-contributors)
- [GitHub flow](https://docs.github.com/en/get-started/using-github/github-flow)
- [Issue and PR templates](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/about-issue-and-pull-request-templates)
- [Reviewing pull requests](https://docs.github.com/en/pull-requests/how-tos/review-pull-requests)
- [Rulesets](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-rulesets/about-rulesets)
- [Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases) and [generated release notes](https://docs.github.com/en/repositories/releasing-projects-on-github/automatically-generated-release-notes)

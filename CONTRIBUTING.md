# Contributing

EmbodiedUE contains Python training code, a UE 5.8 C++ plugin, and binary content.
Start with [setup](README.md#getting-started), [architecture](docs/architecture.md), and the [domain glossary](CONTEXT.md).

Original project code, documentation, and configuration use [Apache-2.0](LICENSE).
Contributions intentionally submitted for inclusion are covered by that license,
unless explicitly stated otherwise and agreed with the maintainer. Submit only work
you have the right to contribute; retain third-party notices and identify any separate
licenses. Asset contributions need their own provenance and redistribution permission.
See [third-party notices](THIRD_PARTY_NOTICES.md) and [release readiness](docs/release-readiness.md).

The project is **Early-Stage**.
Interfaces and procedures can change without a backward-compatibility guarantee.

## Propose a focused change

Use the [contribution workflow](docs/contribution-workflow.md) for proposals, branches, PRs, validation, review, merge, and release decisions.

Describe the problem, expected behavior, and reproduction procedure.
For a bug, include the Task, commit, versions, command, and redacted logs.
For a design change, explain the training or deployment contract and a representative use case.

Keep the repair within the current product scope.
For a new robot, use assets and Python declarations with the existing runtime.
For plan operators, obey the [operator admission requirements](engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md).
These include Python and C++ implementations, reviewed parity data, and a negative self-check.

## Develop locally

Run these commands in your development checkout:

```powershell
uv sync --locked
uv run pytest -q
```

Use [Write tests](docs/how-to/write-tests.md) to select a layer and write the assertions.
Then complete the applicable checks in [tests/README.md](tests/README.md).
Python tests do not establish correct Chaos behavior.
For UE changes, build the Editor and run the applicable Automation and E2E cases on Windows.
Record each unavailable check.

The repository has pre-commit and pre-push hooks for Ruff, Mypy, and tests.
Full pre-push checks can require UE.
Report an unavailable runtime check as blocked.

## Documentation and examples

Write public documentation in English.
Use short sentences and consistent project terms.
Put prerequisites before instructions.
Put expected results next to the related action.

Use repository-relative links and reproducible commands.
Use placeholders for host-specific paths.
Keep first-use instructions in the README, detailed procedures in `docs/how-to/`, and shared contracts in `CONTEXT.md`.
Keep experiment reports and development journals out of public product guides.
Measurements from one host do not establish compatibility or performance guarantees.

For documentation changes, do these checks:

- Verify relative links and anchors.
- Compare commands and flags with CLI help or their parser definitions.
- Compare Task IDs with `uerl tasks` or the registry.
- Compare defaults with resolved configurations.

Do not start training solely for a documentation change.

## Review evidence

Describe the problem, final behavior, completed validation, and remaining limitations.
For an experiment, retain the configuration, seed, commit, dirty diff, manifest, checkpoint identity, and evaluation procedure together.
Generated `runs/` output is not source content.
Agree on a location before you share large artifacts.

Keep credentials, private host paths, confidential logs, and unauthorized assets out of contributions.
Git LFS applies to `.uasset` and `.umap` files.
Before you add binary content, obtain the applicable redistribution permission.

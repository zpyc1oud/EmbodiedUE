# Contributing

EmbodiedUE combines Python training code with a UE 5.8 C++ plugin and binary content. Start with [setup](README.md#getting-started), [architecture](docs/architecture.md), and the [domain glossary](CONTEXT.md).

The project license and contribution terms are still pending owner review. Before submitting third-party code or assets, resolve their provenance and redistribution terms with the maintainer. See [release readiness](docs/release-readiness.md).

## Propose a focused change

Describe the concrete problem, the expected behavior, and how to reproduce it. For bugs, include the task, commit, versions, command, and redacted logs. For design changes, explain the relevant training/deployment contract and a representative use case.

Keep fixes within the current product scope. New robot integrations should add assets and Python declarations through the existing runtime. Changes to plan operators must follow [operator admission](engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md), including Python/C++ implementations, reviewed parity data, and a negative self-check.

## Develop locally

```powershell
uv sync --locked
uv run pytest -q
```

Use the smallest meaningful test layer for the change, then complete the relevant checks in [tests/README.md](tests/README.md). Python tests do not establish Chaos correctness. UE changes require an Editor build and the applicable Automation/E2E cases on the Windows host. State explicitly when those checks were unavailable.

The repository includes local pre-commit/pre-push hooks for Ruff, Mypy, and tests. Full pre-push checks can require UE. Do not describe an unavailable runtime check as passed.

## Documentation and examples

Write public documentation in English. Use repository-relative links, reproducible commands, and placeholders for machine-specific paths. Keep the README focused on first use; place detailed procedures in `docs/how-to/` and shared contracts in `CONTEXT.md`. Keep experiment reports and development journals out of the public product guides. Avoid turning one machine’s measurements into compatibility or performance guarantees.

For documentation changes, check local links and anchors, command/flag names against CLI help, task IDs against `uerl tasks`, and defaults against resolved configurations. Do not start training just to validate a documentation edit.

## Review evidence

A change description should explain the problem and final behavior, list meaningful validation, and identify remaining limitations. For experiments, retain the configuration, seed, commit and local diff, manifest, checkpoint identity, and evaluation procedure together. Generated `runs/` output is not part of the source checkout; agree on an appropriate artifact location before sharing large files.

Do not include credentials, private machine paths, raw confidential logs, or assets without redistribution permission. Git LFS is configured for `.uasset` and `.umap`; confirm a binary contribution is authorized before adding it.

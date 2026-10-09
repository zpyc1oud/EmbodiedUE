# Contributing

EmbodiedUE helps game developers train and deploy physics-driven robots in Unreal Engine.
It contains Python training code, a UE 5.8 C++ plugin, and binary content.
Read the [product direction](docs/product-direction.md) before proposing a new feature.
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
For several reviewed features, use [batch validation](docs/how-to/batch-validation.md).
Keep basic checks in feature development and share expensive UE checks on a frozen integration candidate.
Keep the validation worktree and compatible build caches between batches.

The repository has pre-commit and pre-push hooks for Ruff, Mypy, and tests.
Full pre-push checks can require UE.
Report an unavailable runtime check as blocked.

## Documentation and examples

Write the main documentation in English.
Keep `README.zh-CN.md` aligned with the English README when first-use steps or product claims change.
Both READMEs must use the same commands, Task IDs, defaults, and support scope.
Use short sentences and consistent project terms.
Put prerequisites before instructions.
Put expected results next to the related action.

Use repository-relative links and reproducible commands.
Use placeholders for host-specific paths.
Keep first-use instructions in the README, detailed procedures in `docs/how-to/`, and shared contracts in `CONTEXT.md`.
Keep experiment reports and development journals out of public product guides.
Measurements from one host do not establish compatibility or performance guarantees.

For documentation changes, do these checks:

- Make sure that relative links and anchors resolve.
- Compare commands and flags with CLI help or their parser definitions.
- Compare Task IDs with `uerl tasks` or the registry.
- Compare defaults with resolved configurations.

Do not start training solely for a documentation change.

### Organize documentation for the reader

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

## Review evidence

Describe the problem, final behavior, completed validation, and remaining limitations.
For an experiment, keep the configuration, seed, commit, dirty diff, manifest, checkpoint identity, and evaluation procedure together.
Generated `runs/` output is not source content.
Agree on a location before you share large artifacts.

Keep credentials, private host paths, confidential logs, and unauthorized assets out of contributions.
Git LFS applies to `.uasset` and `.umap` files.
Before you add binary content, obtain the applicable redistribution permission.

## Check documentation

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

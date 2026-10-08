# Documentation

Start with the [repository README](../README.md) for prerequisites and the first training run.
Use this index to find the guide for each topic.

| Topic | Guide |
|---|---|
| Installation and first use | [README](../README.md#getting-started) |
| System design and reasons | [Architecture](architecture.md) |
| Domain terms and timing contracts | [CONTEXT](../CONTEXT.md) |
| Task defaults and command-line overrides | [Configuration](configuration.md) |
| Training, evaluation, video, and robots | [How-to guides](how-to/README.md) |
| In-game inference and packaging | [Deployment](in-game-deployment-guide.md) |
| Setup and runtime failures | [Troubleshooting](troubleshooting.md) |
| Test layers and commands | [Tests](../tests/README.md) |
| Test implementation and review | [Write tests](how-to/write-tests.md) |
| Test audit and Isaac Lab comparison | [Testing audit](testing-audit-2026-10.md) |
| New plan operators | [Operator admission](../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md) |
| Contributions and review | [Contributing](../CONTRIBUTING.md) |
| Proposals, PRs, merge, and release | [Contribution workflow](contribution-workflow.md) |
| Batch integration and shared UE checks | [Batch validation](how-to/batch-validation.md) |
| Trust boundaries and vulnerability reports | [Security](../SECURITY.md) |
| Third-party code and content | [Notices and inventory](../THIRD_PARTY_NOTICES.md) |
| Fab asset removal plan | [Asset publication plan](asset-publication-plan.md) |
| Licenses, assets, and release decisions | [Release readiness](release-readiness.md) |
| Change history | [Changelog](../CHANGELOG.md) |

Videos in `docs/media/` show historical inference.
The guides do not require generated Runs or private reference checkouts.
Those files are not included as experiment evidence.

## Development planning

- [Task and runtime stabilization](design/task-runtime-stabilization.md): Isaac Lab comparison, configured input references, and bounded runtime verification.

- [Repository-wide test quality design](design/test-quality-implementation.md): proposed per-layer changes, independent oracles, implementation slices, and acceptance for Issue #29.
- [Next release roadmap](roadmap/next-release.md): proposed goals, scope, stages, and release conditions.
- [RFC 0001: Developer workflow](rfcs/0001-developer-workflow.md): user operations, architecture, design choices, migration, and validation. Status: Accepted.

After design review, record implementation progress in the related GitHub Issues and PRs.

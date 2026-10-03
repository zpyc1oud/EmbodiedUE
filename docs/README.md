# Documentation

Start with the [repository README](../README.md) for prerequisites and the first training run. Use this index to find the authoritative guide for each topic.

| Topic | Guide |
|---|---|
| Installation and quick start | [README](../README.md#getting-started) |
| System design and technical rationale | [Architecture](architecture.md) |
| Domain vocabulary and timing contracts | [CONTEXT](../CONTEXT.md) |
| Task defaults and command-line overrides | [Configuration](configuration.md) |
| Training, evaluation, recording, new robots | [How-to guides](how-to/README.md) |
| In-game inference and packaging | [Deployment](in-game-deployment-guide.md) |
| Common setup and runtime failures | [Troubleshooting](troubleshooting.md) |
| Test layers and commands | [Tests](../tests/README.md) |
| Adding plan operators | [Operator admission](../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md) |
| Contributions and change review | [Contributing](../CONTRIBUTING.md) |
| Trust boundaries and vulnerability reports | [Security](../SECURITY.md) |
| Third-party code and content | [Notices and inventory](../THIRD_PARTY_NOTICES.md) |
| Fab asset removal planning | [Asset publication plan](asset-publication-plan.md) |
| Licensing, assets, and release decisions | [Release readiness](release-readiness.md) |
| Change history | [Changelog](../CHANGELOG.md) |

Videos in `docs/media/` show historical inference. Generated runs and private reference checkouts are not required to read these guides and are not bundled experiment evidence.

## Development proposals

These documents describe planned work, not current product features or measured results.

- [Development roadmap](development/roadmap.md): architecture changes, Task entry points, onboarding, dependencies, and release scope.
- [Training benchmark design](development/training-benchmarks.md): workloads, timing, learning quality, statistics, and stability.
- [Training and deployment compatibility](development/train-deploy-parity.md): shared semantics, runtime differences, diagnostics, and target-map validation.

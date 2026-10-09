# Distribution requirements

The owner selected **Apache-2.0** for original project code, documentation, and configuration.
The selected project status is **Early-Stage**.
[LICENSE](../LICENSE) contains the unchanged Apache text.
Third-party rights remain separate.
Review the actual source and binary bundle before a release.
Publication requires a separate maintainer decision.

## Remaining publication decisions

| Item | Verified state | Required decision or verification |
|---|---|---|
| Project licensing | Apache-2.0 text, package metadata and contribution guidance are present | Confirm rights over original contributions and the preferred copyright-holder attribution; no legal name was inferred from Git identity. The generic placeholders in the official license appendix are part of its unmodified text |
| Third-party notices | [Preliminary inventory](../THIRD_PARTY_NOTICES.md) records inspected dependency licenses and asset uncertainties | Complete provenance review; include actual upstream notices/licenses for any components bundled in a release |
| Fab content | Owner confirms `engine/Content/Stylized_Egypt/` originated on Fab | 304 raw files excluded; optional [local installation](deployment.md#optional-egypt-demo) retains integration. Current Free price does not settle license/use rights; see [history plan](release-readiness.md#remaining-publication-decisions) |
| Other UE/robot content | Template-like collections, robot assets, policies and media remain present | Determine per-collection source, rights and attribution; do not treat Apache-2.0 as covering unverified content |
| History and LFS | Fab assets are excluded from current tree but present in tracked history and LFS metadata | Choose a rights-cleared publication snapshot or an explicitly authorized history/LFS cleanup; deleting only current paths would not clear historical distribution |
| Security reporting | [SECURITY.md](../SECURITY.md) lacks a designated private reporting channel | Owner supplies a private contact or enables private reporting before publication |

## Early-Stage validation and distribution readiness

The release review still requires clean-host Windows/UE build, live Chaos training smoke, policy import, and packaged-game checks.
These validation limits do not change the selected code license.
Make sure that authorized assets are available through LFS.
Document optional external content so that default examples remain usable.

UE 5.8 is a separately acquired prerequisite.
A source release of original project code does not include rights to engine binaries, engine source or Epic/Fab content.
Review applicable engine/content terms for the distribution actually planned.

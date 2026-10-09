# Release-readiness inventory

The owner selected **Apache-2.0** for original project code, documentation, and configuration.
The selected project status is **Early-Stage**.
[LICENSE](../LICENSE) contains the unchanged Apache text.
Third-party rights remain separate.
The recorded content review does not clear the complete bundle for public redistribution.
The inventory itself does not authorize publication.

## Remaining publication decisions

| Item | Verified state | Required decision or verification |
|---|---|---|
| Project licensing | Apache-2.0 text, package metadata and contribution guidance are present | Confirm rights over original contributions and the preferred copyright-holder attribution; no legal name was inferred from Git identity. The generic placeholders in the official license appendix are part of its unmodified text |
| Third-party notices | [Preliminary inventory](../THIRD_PARTY_NOTICES.md) records inspected dependency licenses and asset uncertainties | Complete provenance review; include actual upstream notices/licenses for any components bundled in a release |
| Fab content | Owner confirms `engine/Content/Stylized_Egypt/` originated on Fab | 304 raw files excluded; optional [local installation](how-to/optional-egypt-demo.md) retains integration. Current Free price does not settle license/use rights; see [history plan](asset-publication-plan.md) |
| Other UE/robot content | Template-like collections, robot assets, policies and media remain present | Determine per-collection source, rights and attribution; do not treat Apache-2.0 as covering unverified content |
| History and LFS | Fab assets are excluded from current tree but present in tracked history and LFS metadata | Choose a rights-cleared publication snapshot or an explicitly authorized history/LFS cleanup; deleting only current paths would not clear historical distribution |
| Security reporting | [SECURITY.md](../SECURITY.md) lacks a designated private reporting channel | Owner supplies a private contact or enables private reporting before publication |

## General platform release gates

The [platform roadmap](roadmap/next-release.md) defines a reusable training and
deployment platform, reproducible examples, and a separate public game demo.
Completion of locomotion Issue #41 and deployment Issue #8 does not clear the
complete release. Extension contracts, physical hit recovery, packaged execution,
independent setup, content inventory, documentation, and presentation also require
evidence.

Keep source-release rights and packaged-game rights separate. Record the actual
files in each deliverable and verify the applicable permissions and notices.
The current project license does not clear an unverified asset or its history.
The roadmap is a plan, not a new publication or security-setting authorization.

## Early-Stage validation and distribution readiness

The release review still requires clean-host Windows/UE build, live Chaos training smoke, policy import, and packaged-game checks.
These validation limits do not change the selected code license.
Make sure that authorized assets are available through LFS.
Document optional external content so that default examples remain usable.

UE 5.8 is a separately acquired prerequisite.
A source release of original project code does not include rights to engine binaries, engine source or Epic/Fab content.
Review applicable engine/content terms for the distribution actually planned.

## Inspection scope

The recorded review covers tracked paths, text references, Git/LFS metadata, selected installed dependency licenses, and owner-confirmed Fab provenance.
It is not a complete legal review, binary dependency analysis, or full-history secret audit.
No credentials were displayed or tested in that review.
Earlier credential cleanup is not a new exposure report.
Revocation status is outside the review.

The Fab collection was removed recoverably from the current tree.
That operation did not rewrite history, purge LFS objects, or publish the repository.

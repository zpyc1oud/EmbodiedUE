# Release-readiness inventory

The owner selected **Apache-2.0** for original project code, documentation, and
configuration, and **Early-Stage** status. [LICENSE](../LICENSE) contains the unmodified
Apache text. Third-party rights remain separate; the current content bundle is not
cleared for public redistribution. These local changes do not publish the repository.

## Remaining publication decisions

| Item | Verified state | Required decision or verification |
|---|---|---|
| Project licensing | Apache-2.0 text, package metadata and contribution guidance are present | Confirm rights over original contributions and the preferred copyright-holder attribution; no legal name was inferred from Git identity. The generic placeholders in the official license appendix are part of its unmodified text |
| Third-party notices | [Preliminary inventory](../THIRD_PARTY_NOTICES.md) records inspected dependency licenses and asset uncertainties | Complete provenance review; include actual upstream notices/licenses for any components bundled in a release |
| Fab content | Owner confirms `engine/Content/Stylized_Egypt/` originated on Fab | Establish exact listing/acquired terms and public editable-asset redistribution rights, or exclude it; see [removal plan](asset-publication-plan.md) |
| Other UE/robot content | Template-like collections, robot assets, policies and media remain present | Determine per-collection source, rights and attribution; do not treat Apache-2.0 as covering unverified content |
| History and LFS | Assets are present in tracked history and LFS metadata | Choose a rights-cleared publication snapshot or an explicitly authorized history/LFS cleanup; deleting only current paths would not clear historical distribution |
| Security reporting | [SECURITY.md](../SECURITY.md) lacks a designated private reporting channel | Owner supplies a private contact or enables private reporting before publication |

## Early-Stage validation and distribution readiness

Clean-host Windows/UE build, live Chaos training smoke, policy import and packaged-game
checks remain outstanding. These are validation limits, not reasons to change the
chosen code license. Verify LFS availability for authorized assets and document
optional externally obtained content so default examples remain usable.

UE 5.8 is a separately acquired prerequisite. A source release of original project
code does not include rights to engine binaries, engine source or Epic/Fab content.
Review applicable engine/content terms for the distribution actually planned.

## Inspection scope

The current review covers tracked paths, textual references, Git/LFS metadata,
selected installed dependency licenses, and owner-confirmed Fab provenance. It is
not an exhaustive legal review, binary asset dependency analysis, or full-history
secret audit. No credentials were displayed or tested. Earlier credential cleanup
is not being reported as a new exposure; revocation status is outside this review.
No assets were deleted, history rewritten, LFS objects purged, or publication action taken.

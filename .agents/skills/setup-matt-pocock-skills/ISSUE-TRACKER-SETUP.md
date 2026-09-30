# Issue tracker setup

Choose the tracker from the repository remote when possible:

- GitHub remote → GitHub Issues using `issue-tracker-github.md`.
- GitLab remote → GitLab Issues using `issue-tracker-gitlab.md`.
- No remote or a solo local workflow → local Markdown using `issue-tracker-local.md`.
- Another tracker → record the user's described workflow in `docs/agents/issue-tracker.md`.

The GitHub and GitLab templates keep the “PRs as a request surface” flag off by default. Do not change that flag unless the user asks for external PRs or merge requests to enter triage.

Write the selected template to `docs/agents/issue-tracker.md` after the common draft review.

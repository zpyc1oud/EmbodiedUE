# Triage model

Issues have one category role and one state role. Categories are `bug` and `enhancement`. States are:

- `needs-triage` — the maintainer needs to evaluate it.
- `needs-info` — waiting for the reporter.
- `ready-for-agent` — an agent brief is complete.
- `ready-for-human` — human implementation or judgment is required.
- `wontfix` — the work will not be actioned.

The configured label mapping supplies the actual strings. An unlabeled issue normally enters `needs-triage`; it can move to `needs-info`, `ready-for-agent`, `ready-for-human`, or `wontfix`. Reporter activity can return `needs-info` to `needs-triage`. Conflicting state labels or an unusual transition need maintainer attention before changing state.

When external PRs are enabled by the issue-tracker configuration, apply the same roles to their attached code. `ready-for-agent` means the next step on the diff is specified; `ready-for-human` means the diff is ready for a human to merge.

# List work needing attention

Query the configured tracker and show three buckets, oldest first:

1. Unlabeled issues or in-scope external PRs.
2. Items in `needs-triage`.
3. Items in `needs-info` with reporter activity since the last triage notes.

When PRs are in scope, label each result `[PR]` or `[issue]` and include only external PRs for discovery. An explicitly named PR is always eligible for triage.

Show a count and one-line summary for each item, then let the maintainer choose what to inspect.

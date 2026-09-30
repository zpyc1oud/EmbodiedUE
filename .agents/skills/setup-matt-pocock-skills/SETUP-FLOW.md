# Setup flow

Inspect the repository's existing configuration before proposing changes. Check the remote and `.git/config`, root `AGENTS.md`/`CLAUDE.md`, context and ADR locations, `docs/agents/`, `.scratch/`, whether `/triage` is installed, and monorepo signals. Read only the files relevant to the configuration branches being considered.

Summarize what exists and what is missing. Make the tracker, label, and domain-layout decisions one at a time when a choice remains; skip a branch already settled by the repository. Show drafts of the files and the `## Agent skills` block before writing.

When writing the shared block:

- Edit `CLAUDE.md` when it exists; otherwise edit `AGENTS.md` when it exists.
- If neither exists, ask which one the user wants to create.
- Update an existing `## Agent skills` block in place and preserve surrounding edits.
- Create configuration and documentation lazily; do not add empty sections or placeholder files.

Finish by naming the files written and the skills that consume them. Users can edit `docs/agents/*.md` directly later; repeat setup only when the tracker or layout changes.

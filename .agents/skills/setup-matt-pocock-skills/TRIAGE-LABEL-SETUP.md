# Triage label setup

Use this branch only when `/triage` is installed. Ask whether the default label strings should be kept. If yes, write the five canonical mappings as-is: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, and `wontfix`. If no, collect the user's existing label names and map each canonical role to one of them.

Write the mapping to `docs/agents/triage-labels.md` and include its `### Triage labels` block in the shared agent-skills section. Omit both when triage is not installed.

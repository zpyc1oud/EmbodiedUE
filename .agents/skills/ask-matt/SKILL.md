---
name: ask-matt
description: Ask which skill or flow fits your situation. A router over the skills in this repo.
disable-model-invocation: true
---

# Ask Matt

Use this file as the index when you are unsure which engineering skill fits. Choose the smallest flow that matches the user's goal; read [WORKFLOWS.md](WORKFLOWS.md) only when the relationships between flows matter.

## Choose by situation

- Shape an idea in a working directory → `/grill-with-docs`; without a working directory → `/grill-me`.
- Answer a runnable logic or UI question → `/prototype`; use `/handoff` only when the prototype must move to another directory or harness.
- Turn a settled multi-session effort into work → `/to-spec`, then `/to-tickets`, then `/implement` as appropriate.
- Work on a hard bug or performance regression → `/diagnosing-bugs`.
- Plan a large effort whose route is still unclear → `/wayfinder`.
- Triage incoming issues or external PRs → `/triage`.
- Improve an existing module's shape → `/improve-codebase-architecture` and `/codebase-design`.
- Sharpen domain language or record a durable decision → `/domain-modeling`.
- Review changes against a fixed point → `/code-review`.
- Research a question from primary sources → `/research`.
- Resolve an in-progress merge or rebase → `/resolving-merge-conflicts`.
- Perform human-only setup or migration steps → `/wizard`.
- Learn a concept over multiple sessions → `/teach`.
- Write a questionnaire for someone else → `/to-questionnaire`.
- Re-explain a message that did not land → `/wait-what`.

Run `/setup-matt-pocock-skills` once before the engineering flows if the repository has not configured its tracker and domain-document locations. Use `/tdd` directly when test-first work is the goal. `/implement` may use `/tdd` and `/code-review` when the change and project workflow call for them; neither is an automatic requirement for every task.

# Triage a specific issue or PR

Resolve a bare issue number according to the configured tracker. Read the full issue or PR: body, comments, labels, author, dates, and, for a PR, the diff. Parse prior triage notes so resolved questions are not repeated. Explore the relevant code using the project's glossary and ADRs.

Before recommending a state, check two claims against the codebase:

- **Redundancy:** search by domain concept for an existing implementation and report where you looked.
- **Prior rejection:** inspect `.out-of-scope/*.md` for a similar rejected request.

Recommend a category and state with the relevant codebase context, including whether the behavior already exists. Wait for the maintainer's direction before applying a recommendation.

Verify the claim before writing a brief. Reproduce a bug from the reporter's steps. For a PR, check out the diff and run the relevant tests or commands. Report confirmed, failed, or insufficient detail. If the request needs clarification, use `/grilling` and `/domain-modeling`.

Apply the maintainer's outcome:

- `ready-for-agent` → post the [AGENT-BRIEF.md](AGENT-BRIEF.md) structure.
- `ready-for-human` → use the same structure and explain why delegation is unsuitable.
- `needs-info` → post specific triage notes describing established facts and missing answers.
- `wontfix` → close it; record rejected enhancements in [OUT-OF-SCOPE.md](OUT-OF-SCOPE.md), but point to existing code when the behavior is already implemented.
- `needs-triage` → apply the role and add a progress note only when useful.

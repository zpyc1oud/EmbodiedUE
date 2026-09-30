# Workflow notes

Use these relationships only when a task crosses more than one skill.

## Idea to implementation

Shape the idea first. If a design question needs a runnable answer, use `/prototype` and return the learning to the main task. If the effort spans sessions, turn the settled conversation into `/to-spec`, split it with `/to-tickets`, and implement the resulting work. For a small effort, `/implement` can work directly from the conversation or spec.

Use `/tdd` when test-first behavior is the point. Use `/code-review` when a fixed-point review is required or useful. Choose both based on the change and repository workflow, not by default for every edit.

## Other entry points

- Incoming raw requests go through `/triage`; hard failures go through `/diagnosing-bugs`.
- Large, unclear efforts go through `/wayfinder` and return to `/to-spec` when the route is clear.
- Architecture upkeep starts with `/improve-codebase-architecture`; a selected candidate can continue through `/grill-with-docs`.
- `/domain-modeling` and `/codebase-design` provide vocabulary when language or module shape is the problem.

Use `/handoff` for portability, not as a routine phase transition. See [PHASE-BOUNDARIES.md](PHASE-BOUNDARIES.md) for the context decision.

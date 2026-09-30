# Feedback loops

Choose the simplest signal that reaches the real bug path and can distinguish the user's symptom. The options below are examples, not a required order:

- A failing unit, integration, or end-to-end test at the seam that reaches the bug.
- A curl or HTTP script against a running development server.
- A CLI invocation with fixture input compared with a known-good result.
- A headless browser script asserting DOM, console, or network behavior.
- A captured trace, request, payload, or event log replayed in isolation.
- A throwaway harness with only the service and dependencies needed for the path.
- A property or fuzz loop when the wrong output depends on input combinations.
- A bisection harness when the failure appeared between known-good states.
- A differential loop comparing versions or configurations.

For a flaky bug, stress the smallest useful trigger, parallelize when useful, or pin time and randomness. For a performance bug, start with a repeatable baseline and measure each change. For a human-in-the-loop repro, use `scripts/hitl-loop.template.sh` so the captured observation returns to the diagnostic loop.

A useful loop is red-capable, relevant to the exact symptom, fast enough to run repeatedly, and runnable without unnecessary manual work.

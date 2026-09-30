---
name: diagnosing-bugs
description: Diagnosis loop for hard bugs and performance regressions. Use when the user says "diagnose"/"debug this", or reports something broken/throwing/failing/slow.
---

# Diagnosing Bugs

Use this skill for bugs that need evidence before a fix, especially intermittent failures and performance regressions. Start by finding a tight feedback loop that reaches the user's actual symptom; choose the loop and investigation depth that fit the bug.

When exploring the relevant code, read `CONTEXT.md` if it exists and check ADRs in the area being touched.

## Choose a feedback loop

- Existing test, CLI, HTTP, browser, trace replay, or a small harness → see [FEEDBACK-LOOPS.md](FEEDBACK-LOOPS.md).
- Non-deterministic behavior → increase the reproduction rate and make the signal as deterministic as the bug allows.
- Performance regression → measure a baseline and use profiling or a timing harness before changing code.
- Human interaction is required → use [scripts/hitl-loop.template.sh](scripts/hitl-loop.template.sh).

## Invariants

- The signal must drive the real bug path and assert the user's exact symptom, not merely avoid an exception.
- Prefer a fast, deterministic, unattended loop. If no such loop is possible, record what is missing and ask for the environment, a redacted artifact, or permission for temporary production instrumentation.
- Redact secrets from commands, output, and captured artifacts before showing them.

Use [DIAGNOSTIC-LOOP.md](DIAGNOSTIC-LOOP.md) for the evidence cycle and its completion criteria. It is a guide, not a mandatory itinerary; adapt it when the evidence makes a branch irrelevant.

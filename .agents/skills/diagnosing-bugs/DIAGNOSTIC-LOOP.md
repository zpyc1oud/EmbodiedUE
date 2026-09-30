# Diagnostic loop

Run the parts that the evidence calls for. The goal is a conclusion supported by a repeatable signal, not completion of a ceremonial checklist.

## Reproduce and reduce

Run the loop and capture the exact error, output, or timing. If useful, reduce inputs, callers, configuration, data, and steps while preserving the failure. Keep each remaining element because removing it changes the verdict.

## Form and test explanations

Create enough competing, ranked hypotheses to avoid anchoring. Each one needs a prediction that a probe can distinguish. Share the list with the user when their domain knowledge could re-rank it, and continue if no response is needed to make progress.

Make each probe answer one question. Prefer a debugger or REPL, then targeted logs at boundaries that distinguish hypotheses. Tag temporary debug logs and remove them after the investigation. Measure performance rather than inferring it from logs.

## Fix and lock the behavior

When a correct test seam exists, turn the smallest reproduction into a failing regression test, apply the fix, and verify the test passes. If no correct seam exists, record that architectural finding instead of creating a test that gives false confidence. Re-run the original scenario once after the fix.

## Finish

Confirm the symptom is gone, the regression test or missing-seam finding is recorded, temporary instrumentation is removed, and throwaway artifacts are deleted or clearly marked. Record the causal explanation in the commit or PR when that will help the next investigation. If the fix exposes architectural friction, hand it to `/improve-codebase-architecture` after the fix.

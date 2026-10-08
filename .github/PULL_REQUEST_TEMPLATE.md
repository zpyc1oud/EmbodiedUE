## Problem and change

Describe the problem and final behavior.
Link the related Issue or RFC.
Use a closing keyword only when this PR completes its acceptance scope.

## Scope and contracts

List user-visible changes and affected training or deployment contracts.
State compatibility effects, content dependencies, and any required recovery steps.

## Validation

Record the tested commit and any dirty diff.
For each applicable check, give the command, host/tool versions, selected cases, result, and evidence location.

- Passed:
- Failed:
- Skipped, with reasons:
- Blocked, with missing requirements:

Remove unused result entries.
Static checks establish only Ruff/Mypy results.
Mocks and smoke runs do not establish real Chaos behavior or learning quality.
For an integration batch, identify the batch PR and pending UE acceptance.

## Review and remaining work

List blockers, remaining work, and the owner of each missing result.
Confirm that shared logs are redacted and contributed assets have redistribution permission.

Use the [contribution workflow](../docs/contribution-workflow.md) and [test guide](../tests/README.md).
Keep this PR in Draft until its required evidence is ready.

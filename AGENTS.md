# UE RL Engine

Train robot policies in Unreal Engine 5.8 Chaos.
Deploy the same artifact on a Skeletal Mesh in a game.

## Read the applicable guide

- For installation, commands, and directory layout, read `README.md`.
- For domain contracts or timing changes, read `CONTEXT.md`.
- For system boundaries and design decisions, read `docs/architecture.md`.
- For topic-specific documentation, use `docs/README.md`.
- For proposals, validation, review, merge, and release, use `docs/contribution-workflow.md`.
- For robot integration, video, and deployment, use `docs/how-to/README.md`.
- Before you add or change tests, read `docs/how-to/write-tests.md` and `tests/README.md`.

The contribution workflow does not extend the user's action authorization.

## Product contracts

A robot integration adds UE assets, Python Robot and Task declarations, and a policy artifact.
It does not add robot-specific UE runtime code.
The product robots are CartPole and PhantomX.
Training, evaluation, export, and deployment use `uerl`.

A Session owns `physics_dt` and an inclusive `decimation` range.
Each Step carries a `step_decimation` within that range.
Contact observations are geometric support values of 0 or 1.
They describe the final completed solver step at the end of the control window.

## Scope limits

Report actual defects, including rare cases that the project can produce.
These limits control proposed changes, not the search for defects.

1. Assume a cooperating operator on their own machine, unless the project specifies an adversary.
   Use the stated threat model when one exists.
   Add verification where necessary, without unrelated defensive infrastructure.
2. Add a hash, checksum, or fingerprint only if it replaces a materially more expensive operation.
   Its result must also change the next action.
3. Add no feature flags, migration frameworks, compatibility layers, or wrappers for cases that do not occur here.
4. Include unusual inputs only when supported project use can reach them.
   Documented inputs, published interfaces, and real project data establish reachability.
   A reproduction is not required for a reachable case.
   A theoretically constructible case alone is insufficient.
5. Use judgment where judgment is necessary.
   Do not replace a settled decision with a scoring table, checklist, or repeated verification loop.
6. Obey security, migration, verification, and review requirements requested by the user, project conventions, or higher-priority instructions.
   Those requirements remain in scope.
7. State deliverable facts directly.
   Put applicable caveats together under Limitations or Known Issues.
   Do not repeat a prohibition as product text.
   If the user excludes a topic, omit that topic from the deliverable.

## Examples for scope decisions

These examples explain the limits.
They do not justify dismissal of a real defect.

Usually unnecessary:

- A hash for every spreadsheet row when a direct cell comparison gives the required answer
- A checksum file that no operation reads
- Account hardening for an application with no users or deployment
- Repeated patch reviews that prevent delivery
- A reviewer that rejects every result
- A guard required only by another unnecessary guard

Potentially necessary:

- A digest that avoids another read of a large file already available
- An unusual input produced by the project's own documented example

Before each check, identify the specific failure it can detect.
Identify the action that would change after that failure.
If neither answer is available, omit the check.
State when the result is correct.
Do not invent findings.

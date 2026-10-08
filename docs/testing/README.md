# Test-quality review

[Issue #29](https://github.com/zpyc1oud/EmbodiedUE/issues/29) owns the repository-wide assertion review.
[The YAML record](issue29-review.yaml) accounts for 237 tracked test and support files at its recorded baseline.
It includes the external example test and tooling/CI configuration missing from the earlier 231-file inventory.

All 237 files now have full-file review decisions: the previous 16, all 220 requested remaining files,
and one newly found Editor test source outside the Tests directories.
A scan of tracked test names and executable test definitions reconciles that extra source.
The record covers assertions, helpers, runners and complete fixture contents, including binary policy plans and tensors.
The retained source-review archive contains detailed numerical checks; the tracked record summarizes each oracle and consumer.
Runtime acceptance is recorded separately from file-review completion.

The CPU changes retain established numerical tests, remove a redundant local-dictionary identity assertion,
and strengthen saved-artifact binding, distinct reward/action rows and mixed terminal/timeout PPO returns.
Four temporary faults in an isolated copy fail the intended assertion; restored tests pass.
They reverse saved observation members, broadcast the first reward row, omit action clipping,
and use a fixed reference discount for timeout bootstrap.
These demonstrate detection, not four new product defects.

See [Write tests](../how-to/write-tests.md) for the implemented examples and [Tests](../../tests/README.md) for commands.
Run applicable native and real workflow checks when later slices change those contracts.
Keep physical response and learning quality separate from this CPU evidence.

The stateful slice retains the Direct/Manager numerical trajectory oracle and
Bridge/adapter byte and lifecycle checks. It checks noncommuting curriculum terms,
distinct named checkpoint states and sparse event writes against literal values.
Session failure tests preserve the original error when Bridge and Worker cleanup
also fail; cleanup failures remain visible as exception notes.
RNG draws and temporary example imports restore prior process-global state.
Unsupported real-process claims were removed from controlled-transport test output.
These controlled seams do not establish actual Worker or Chaos behavior.


The complete review strengthens distinct action rows, persisted command/trace evidence,
full terrain configuration preservation, exact Task discovery, real artifact rewrites,
and historical fixture-generator timing. It retains reviewed golden values.
The native cases compare both network shapes with independent outputs, check generated
geometry and all atlas origins, guard accesses after failed prerequisites, and restore
physics settings. A self-assignment-only physics test is removed; Worker projection
validation moves to the Unit group.

Runner completion checks reject failed or duplicate Automation identities and require
142 core cases plus 13 terrain/Robot cases. PIEAttach is the separate E2E case.
A real Windows trainer/child timeout test checks owned-tree cleanup; controlled failure
cases verify that cleanup errors do not replace the primary error or prevent other cleanup.
Real E2E now compares the untouched CartPole Slot against a no-reset physical control run.
Terrain cases verify plane/heightfield/boxes observations, retained unselected tiers and episode counters.
Raw RESET payloads zero-fill unselected rows; these are protocol padding, not physical snapshots. The startup event case verifies the actual RESET payload sent through Session.

## Real workflow matrix

The existing E2E matrix is retained. Counts here describe configured Slots, not test totals.

| Task or boundary | Slots | Exercised behavior |
|---|---:|---|
| CartPole Worker | 64 (PIE: 2) | Fixed actions/seed, reset, repeatability, long stepping, protocol failures, ownership, viewport and PIE |
| CartPole formal Session | 2 | Initialize, distinct actions, sparse reset, repeatable manifest and close |
| CartPole DirectEnv | 64 | Batched step, reset, observations, termination and close |
| PhantomX Walk, Pursuit, Continuous Terrain, Discrete Terrain | 1 each | Four-step rollout, one training iteration and checkpoint |
| PhantomX Walk artifact | 1 | Train, supported export, real artifact reload and plan validation |
| PhantomX terrain/contact | 64 | Contact fields, terminal timeout, position targets and startup terrain levels |
| PhantomX terrain reset | 2 | All three primitives, distinct levels and unaffected Slot observations |
| Installed external Direct Task | 2 | One-iteration train, strict resume, 32-step play and unsupported export rejection before Worker launch |

Pursuit uses its authorized configured content; missing content blocks that case.
The installed external-package checks build real distributions and run from an isolated target.
External Direct Task export is explicitly unsupported and tested before Worker startup.
Strict checkpoint continuation and finite-play CLI behavior retain their separate integration
and recorded real-workflow evidence. A one-iteration smoke does not establish learning quality;
that acceptance remains in Issue #8.


## Completed acceptance (2026-10-08)

The final code candidate is `709613dbd37794ca5df72bad091e8a884d2a3513`.
The review baseline is `282ace1600ba27c080cfa8bdad4a70025d3cd98d`; the candidate
also incorporates main's contribution templates at `1bb3b19`.
Subsequent acceptance-record edits change documentation only.
Windows 11 validation used Python 3.11.15, UE 5.8.3 CL 58210709,
PyTorch 2.11.0+cu128, pytest 9.1.1 and rsl-rl-lib 5.4.2.

| Layer | Command or operation | Actual result |
|---|---|---|
| Default Python | `python -m pytest -q` | 1034 passed, 2 platform skips, 0 failed; exit 0 on final code |
| Static checks | Ruff, Mypy and Linux-target Mypy | Passed; Mypy checks 259 files; final-code GitHub static CI passed |
| Editor | UBT Development Editor build | Initial 63-action build passed; final-code 4-action Editor rebuild passed |
| Native Automation | Full runner core and terrain/Robot filters | 142 + 13 passed; exact completed identities reconciled against registered cases |
| Changed Editor cases | `UERL.Unit.Policy.Editor` filter after final rebuild | All 5 passed; these are reruns within the 155, not extra unique cases |
| Real E2E | `python scripts/run_e2e.py --suite all --host-profile <profile> --output-dir <output> --timeout 1200` | 25 passed, 0 failed/skipped in one complete invocation; exit 0; includes PIEAttach |
| Installed package | Isolated installed framework and generated external Task | Train, resume and 32-step play exited 0; unsupported export exited 1 before Worker launch as expected |
| Windows ownership | Actual trainer/descendant timeout and affected cleanup file | 27 passed, 1 Linux-only skip; no surviving owned child |

The 155-case native run used `2009096`, with the same final native source except
the additional Editor prerequisite guard. The final rebuild and all five Editor
cases validate that delta. Together with the separate E2E PIEAttach case, all
156 registered native identities have success evidence. Deployment component,
clock, controller and corpus parity checks establish their named behavior;
short training does not establish learned policy quality.

The first complete runner invocation is retained as a failure: Python and both
native stages passed, while E2E had 22 passes and 3 failures. Two new sparse-reset
oracles incorrectly read unselected RESET padding as physical state. Their
replacements inspect the next real Step against an appropriate control or
terrain response. The third failure lacked Pursuit's configured authorized map;
the host's existing content was reused and the original scene passed. Focused
three-case and subsequent complete 25-case runs both passed.

Installed continuation restored the actor, critic, optimizer, iteration,
curriculum and decimation RNG under the existing contract. Optimizer steps
advanced from 20 to 60 and actor parameters changed; finite play completed
32 decisions. Episodes start fresh, and full RNG/physical state are not saved.
No new dependency installation or private-content redistribution was needed.

Per-file review completion is 237/237, including all 220 requested files plus
the newly discovered Editor source and the 16 previously reviewed files.
Both source-review axes have zero unresolved findings. Raw commands, revision
and dirty-state records, complete logs, YAML summaries, completed native names,
checkpoint evidence and process identities remain in the local verification
archive. UE-generated configuration output was backed up and excluded from the
candidate. The two default-suite skips are Windows symlink privilege and
Linux `/proc` inspection. Learning-quality acceptance remains in Issue #8.

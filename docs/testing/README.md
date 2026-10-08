# Test-quality review

[Issue #29](https://github.com/zpyc1oud/EmbodiedUE/issues/29) owns the repository-wide assertion review.
[The YAML record](issue29-review.yaml) accounts for 236 tracked test and support files at its recorded baseline.
It includes the external example test and tooling/CI configuration missing from the earlier 231-file inventory.

All 236 files now have full-file review decisions: the previous 16 plus 220 completed in this round.
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
Real E2E now checks sparse reset state, plane/heightfield/boxes observations and unchanged
Slots. The startup event case verifies the actual RESET payload sent through Session.

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

Pursuit uses its authorized configured content; missing content blocks that case.
The installed external-package checks build real distributions and run from an isolated target.
External Direct Task export is explicitly unsupported and tested before Worker startup.
Strict checkpoint continuation and finite-play CLI behavior retain their separate integration
and recorded real-workflow evidence. A one-iteration smoke does not establish learning quality;
that acceptance remains in Issue #8.

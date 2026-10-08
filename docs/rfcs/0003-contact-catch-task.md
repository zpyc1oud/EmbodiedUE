# RFC 0003: Contact-based Catch task

Status: proposed, with an implementation prototype for review.
Related Issue: [#40](https://github.com/zpyc1oud/EmbodiedUE/issues/40).
Depends on the command-tracking work in RFC 0002.

## Behavior

The robot pursues a moving human-shaped target in a redistributable arena.
Capture requires a blocking physical hit between that target and the controlled
robot. Distance alone cannot report capture. Capture ends the episode; reset
clears the capture state and restores both participants.

The task reuses the velocity-conditioned locomotion observation and action plans.
A target command source computes a body-frame forward and yaw command. It does
not stop at the former Pursuit stopping distance. The Python task adds capture
reward and termination. This is command-guided locomotion with a capture objective.
It does not claim a new vision-based or target-conditioned end-to-end policy.

The initial capture bonus is 1000. With the default 20 ms discount of approximately
0.994 and a maximum positive locomotion reward of 4 per reference interval, the
infinite-horizon locomotion ceiling is approximately 665. A bonus above that ceiling
prevents delaying an available capture merely to collect more locomotion reward.
This is a default-objective calculation, not a learning result. Changes to discount
or positive reward weights require the same comparison and fresh validation.

The first supported scene has one robot and one target. This limit is explicit.
Training and evaluation can use a deterministic moving target. Interactive use
can bind the target to a player-controlled Pawn. Both paths use the same physical
contact criterion and locomotion input contract.

## Ownership and interfaces

The Environment owns arena geometry, target movement, target reset and target
state. The Robot provider owns its physical actor. Python owns task mathematics.
The Worker owns the active control window.

Add a generic read-only Robot Slot actor accessor for interaction binding. Add
Environment lifecycle hooks for a control window and its physical frames. Normal
Environments use empty hooks. Catch binds only its own controlled Slot actor.
No PhantomX joint, body name, asset path or task reward belongs in the UE hooks.

The reusable hit-listener component lives in UERLProvider, without a dependency
on the training Worker. A target hit listener accepts only a blocking hit from the bound Robot actor while
the Worker owns an active Step. It latches capture until reset. Hits from ground,
unrelated actors, setup, idle world ticks or a previous episode cannot capture.
Reset and destruction disarm and clear the listener. A missing target or Robot is
a reported Slot fault, not a successful capture.

The Environment publishes target position in Slot-local metres and a binary
capture value. These are task state fields, separate from the existing geometric
ground-support observation and its final-solver-step contract.

## Scene and distribution

Use project-owned arena geometry and a primitive human-shaped target for the
first reproducible example. Label it as a proxy, not a scanned human asset.
The scene generator and movement parameters belong in the package. A recipient
must be able to create the example with the supported UE installation.
Optional external demo content remains outside the distributable package.

## Alternatives

A distance threshold is insufficient because it can succeed without contact.
World-wide actor searches during every observation obscure ownership and can
bind the wrong robot. Bind the controlled actor once after successful creation.
Accumulating ordinary ground-contact observations would change their existing
meaning. Use a separate target-interaction event instead.

## Validation before acceptance

- Python: target command direction, stop/capture behavior, independent reward
  values, capture/failure/timeout precedence, registry and configuration loading.
- Native: actual robot-target hit, no capture for a separated target, wrong-actor
  rejection, active-Step ownership, reset clearing and destruction cleanup.
- Real E2E: headless scene creation, target motion under fixed physics, capture
  through actual collision, repeated resets, target loss and process cleanup.
- Deployment: the trained locomotion plans run against the game target binding;
  capture remains a game/scene interaction rather than a network output.
- Learning: measured contact success, capture time, tracking errors and falls on
  held-out target paths. Preserve unsuccessful Runs.

The native build and affected runtime gates must run on the exact candidate.
A fabricated event passed to a listener is only a unit test. It cannot replace
real Chaos contact evidence. Keep this proposal open until that evidence exists.

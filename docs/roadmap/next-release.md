# General platform release roadmap

Status: release plan proposed on 2026-10-09. Implementation is incomplete.
The version number and release date are not assigned.

## Product and release outcome

EmbodiedUE is a reusable platform for robot policy training in Unreal Engine and
policy deployment into games. The public demo is a reference application of this
platform. It must not determine robot-specific behavior inside the framework.

An external developer must be able to configure a host, add a Robot and Task,
select supported inputs, train and evaluate, restore a saved Run, export, and
integrate the policy into a separate UE scene. A game developer must be able to
supply commands and physical interactions without running the Python trainer.

The first release has three deliverables:

1. The Python package and UE plugin, with documented extension contracts.
2. Reproducible CartPole and PhantomX examples, configurations, and policy evidence.
3. An original playable game demo and a short real-time demonstration video.

This is an Early-Stage release with a bounded support statement. Generality means
reusable interfaces with tested extension paths. It does not mean automatic
support for every robot, sensor, Python function, UE version, or game world.

Completing Issue #41, PR #50, and Issue #8 is necessary for the active PhantomX
path, but does not complete platform extension, disturbance recovery, independent
setup, packaged runtime, content clearance, or public delivery.

[RFC 0001](../rfcs/0001-developer-workflow.md) remains the accepted developer
workflow. Use a focused RFC before a new input-provider or other public contract
changes. Reuse existing interfaces where they already meet the requirements.

## Architecture boundaries

| Layer | Owns | Must not depend on |
|---|---|---|
| Framework and UE runtime | Session timing, physics stepping, layouts, reset, plans, inference, lifecycle | PhantomX Task names, a particular map, or the demo's objective |
| Robot and input integrations | Topology, reference pose, actuators, sensor bindings, units and frames | One training objective or the demo's player logic |
| Task package | Commands, observations, rewards, termination, events, curriculum, evaluation | A private copy of the simulation loop |
| Game integration | Navigation, player detection, target selection, interaction, UI, velocity commands | A running Python trainer or direct access to training-only observations |
| Reference applications | CartPole examples, PhantomX examples, the public demo | Undocumented edits to framework internals |

The policy controls joint targets through the exported action plan and Chaos.
Game code can choose routes and commands. It must not silently replace learned
locomotion with root translation or recorded animation in the reference demo.

## Supported first release

Start with the documented Windows x64 and UE 5.8 setup, CartPole and PhantomX,
static ground, and declared control timing. Freeze exact tested versions before
the release candidate. Keep Python-only checks available without UE.

The platform supports minimal Direct Tasks and Manager-composed Tasks through the
same step/reset path. Export remains a separately checked capability. Arbitrary
Python observation/action code must not be described as exportable without a
supported implementation and cross-language evidence.

Required inputs initially include robot state, commands, contact, and supported
ground queries. The extension design must allow later sensor providers, including
vision, without a Task-specific transport path. A camera implementation or a
vision policy is not a first-release requirement.

PhantomX must stand, move forward/backward/laterally, turn while moving, and
recover from defined non-terminal physical hits. Turn-in-place and a new Catch
training Task are outside the selected scope. Full recovery from a fallen pose
is separate from recovering a disturbed walking gait and is not implied.

## Public reference application

Use an original compact industrial map, a third-person player, one PhantomX enemy,
one item, and one exit. The player avoids the robot, retrieves the item, and
reaches the exit. Include clear instructions, a result, and reliable retry.
The selected first version has no shooting system. A simple interaction can
apply a documented physical hit so the viewer can see gait recovery.

The robot can patrol, approach or intercept while moving, stop, and return to its
route. Keep those decisions in the game controller. Define detection, loss, retry,
and navigation-failure rules in the demo design. A simple authored encounter
rule does not require a contact-trained capture policy.

The presentation can evoke a small extraction-game encounter. Use original or
rights-cleared names, layout, art, audio, and interface. Do not distribute another
game's content or imply affiliation. Target a one-to-two-minute playable loop
and a 60-to-90-second real-time video. These are presentation targets, not evidence
that the controller works.

## Existing evidence and gaps

This snapshot records state on 2026-10-09. It is not release certification.

| Area | Current evidence | Remaining gate |
|---|---|---|
| Host, external Tasks, saved Runs | Issues #5, #6, #7 are closed | Independent end-to-end reproduction and current extension-contract audit |
| Tests | Issue #29 is closed | Applicable tests on the exact release candidate |
| Runtime cost | PR #45 is merged | Profile the actual packaged application and supported robot count |
| Evaluation | PR #47 is Ready with runtime evidence | Merge reviewed metrics; keep survival separate from tracking |
| Locomotion | Issue #41 is open; PR #50 is the current candidate | A selected policy with independent seeds and held-out behavior evidence |
| Deployment | Issue #8 has parity and physical-response evidence | Held-out scene, interaction, timing, and packaged-runtime acceptance |
| Shared inputs | Shared plans and current robot/ground inputs exist | Audit provider extension and cross-map binding; close measured gaps |
| Hit recovery | No accepted recovery envelope is established | Matched physical perturbations, controlled training, and held-out recovery tests |
| Public delivery | Code license and preliminary inventory exist | Complete demo, independent setup, actual bundle clearance and documentation |

PR #48 is currently a dependency of #50. Its earlier turn-in-place experiment is
not a release requirement. Preserve evidence while consolidating the final code.
PRs #42, #43, #46, and #49 are closed and unmerged. Do not restore cancelled or
unsuccessful work as required implementation.

Issue #40 still contains cancelled Catch work and overlaps #41. Reconcile its
remaining delivery scope with this plan before using it as an execution checklist.
No issue closes merely because a dependent PR merges.

## Milestones

### M0 Freeze the platform contract and release matrix

Audit the existing Robot, Task, input, Run, artifact, and game-control boundaries.
Record which interfaces already work, which need a repair, and which need a new
extension. Avoid a broad rewrite where the existing contract is sufficient.

Define the supported host, robot examples, input providers, Task entry styles,
export capabilities, maps, timing, and game integration. Record the exact tests
for each supported combination. Make unsupported combinations explicit before
long training. Preserve the shared Session clock and completed-window semantics.

Define the demo's game-level design separately. Freeze its objective, state
transitions, interaction rules, and test scenarios. Record a target host,
resolution, quality preset, robot count, frame-time budget, memory budget, and
soak duration before final performance tests. Assign owners when work is scheduled.

Exit: reviewed support matrix and extension contracts, plus a bounded reference
application design. Public-contract changes have an accepted RFC.

### M1 Prove extension without framework forks

Use external packages and documented registration. Show both a minimal Direct
Task and Manager composition through the common runtime. Change a reward and
configuration from outside the checkout. Retain early capability checks so
unsupported export fails before an expensive training run.

Verify Robot integration by declaration and generic UE components, using CartPole
and PhantomX as distinct reference topologies. A new example must not require a
robot-name branch in the Worker or policy controller.

For each supported input provider, define name, shape, type, units, frame,
configuration, sampling time, reset behavior, missing-data behavior, and deployment
binding. Distinguish ground geometry from the sensor that queries it. Training
and game maps can differ while keeping the policy input contract identical.
Record provider requirements in the artifact or its validated integration contract.

Prove the extension path with an independent example and tests. Check two static
maps with different geometry/bindings. A new native sensor can require a UE plugin
implementation; it must not require a Task-specific core dispatcher. Define how
future image inputs would be admitted without claiming that vision is implemented.

Exit: a developer adds a Task and input integration through documented seams,
with reset/isolation, configuration, capability, export and actual UE evidence
where applicable. Dependency tests prevent reference applications entering core.

### M2 Establish the learned reference behavior

Use Issue #41. Finish the current command-distribution experiment before changing
its parameters. Select feasible velocity ranges for PhantomX from measured
behavior; reference-task ranges do not prove a feasible operating envelope.

Keep Runs, exact source/configuration, seeds, checkpoints, transitions, physical
time, and optimizer budgets. Report standing, forward, backward, lateral, and
moving-turn behavior separately. Include planar/yaw RMSE, signed velocities,
stopping drift, falls, and action saturation. Do not treat survival as success.

Use at least three training seeds for the final selected candidate and separate
held-out evaluation seeds and commands. Freeze behavior thresholds before the
acceptance measurements. Keep failures. Evaluate the exported artifact as well
as the training path. Do not tune acceptance criteria to the final results.

Exit: a named policy and its declared motion envelope pass the frozen protocol.
The complete train, saved-Run, evaluation, export, and game-inference path works.

### M3 Add bounded physical hit recovery

After basic walking is stable, define a reusable perturbation event. Specify the
body, application point, world/body frame, impulse units, direction, magnitude,
timing, and duration or repetition. An impulse at a point can produce translation
and rotation. A velocity assignment or increment is a different operation;
do not call those equivalent without measured evidence.

Use the same declared perturbation meaning in training and game validation.
Select a bounded curriculum from tolerable disturbances. Compare with the
undisturbed baseline so increased robustness does not hide lost locomotion.
Keep the present unperturbed PR #50 experiment unchanged.

Define recovery as return to a specified tracking, attitude, and height band for
a sustained interval after the hit. Freeze bands, timeout, impulse envelope, and
separate safety/fall conditions before held-out tests. Measure recovery rate,
recovery time, peak orientation/position deviation, falls, and velocity error.
Include no-hit controls, both hit sides, different gait phases, and held-out
magnitudes and starts. Report failures outside the supported envelope explicitly.

Exit: actual UE traces show a physical disturbance and sustained recovery under
the declared conditions in both training and the target game. No teleport reset,
animation switch, or hidden root correction counts as gait recovery.

### M4 Validate the independent game integration

Use Issue #8 and the reference-application Issue. Build a greybox first. Connect
patrol and player-response commands to the frozen policy. Clamp commands to its
supported envelope. Define expired-command, navigation-failure, policy-fault,
out-of-domain and restart behavior.

Verify world/body transforms, terrain queries, joint defaults, history, timing,
and reset on held-out static-ground starts and routes. Run same-input parity and
separate closed-loop tests. Test frame-rate variation within supported timing,
player appearance/disappearance, repeated retry, unload/reload, and physical hits.

Complete pickup, exit, loss, result, and retry in the game layer. Test repeated
triggers and cleanup. Add lighting, camera, visual feedback and audio after the
loop passes. A new tester must understand and complete the encounter without
help from the implementation author.

Exit: the reference game uses public platform interfaces, the released policy
controls the robot, and locomotion plus declared hit recovery pass in the actual
map. Preserve the distinction between platform tests and game objective tests.

### M5 Verify the release candidate and public deliverables

Freeze source, assets, policy, dependencies, versions, and maps. Build the real
Windows package. End-user inference must work without a Python trainer or Editor
session. Measure frame-time median/p95/p99, missed control windows, system memory,
GPU memory, and repeated-reset stability against the M0 budgets.

Run applicable Python, native, E2E, parity, lifecycle and packaged-game tests on
the exact candidate. Recheck affected gates after repairs. Training throughput
is not game frame rate. Keep performance, learning quality, recovery, and runtime
correctness as separate results.

Use an independent Windows setup and a developer who did not write the changes.
They must launch the demo and follow the source guide to create an external Task,
change one reward, train the reference, export, and deploy into the named separate
scene. Keep failures and corrective steps. If that setup or tester is unavailable,
the gate stays blocked rather than being replaced with another original-host run.

Complete the [release inventory](../release-readiness.md). Verify the actual
source and binary bundles, including robot assets, policies, media, dependency
notices, historical content, and the documented security channel. UE is a
separately acquired development prerequisite. Do not bundle its installation.
A code license does not clear every content file. Any destructive history change
or new security setting requires separate authorization.

Deliver platform setup/extension guides, supported versions, reproducible training
recipes, game controls, hardware requirements, known limitations, and release
notes. Test downloads and instructions against the candidate. Record an honest
real-time demo with an uncut encounter and hit-recovery segment. Label time
compression or debug views. Do not claim untested capabilities in the trailer.

Exit: platform packages, examples, game build, documentation and media are verified
and ready for the maintainer's separate tagging/publication decision.

## Order and issue structure

M0 and the current M2 experiment can proceed together. M1 closes platform gaps
without waiting for polished game art. Asset inventory and greybox design can
start early. M3 requires stable locomotion; final M4 acceptance requires the
selected policy and hit-recovery envelope. M5 consumes the frozen combined result.

Use one release tracking Issue and reuse #41 for locomotion and #8 for deployment.
Create focused execution Issues for:

1. Platform extension and shared input contracts, covering M0 and M1.
2. Physical perturbation and gait recovery, covering M3.
3. The separate playable reference application, covering game work in M0 and M4.
4. Independent packaged validation and public delivery, covering M5.

Each Issue records dependencies, deliverables, tests, and exit conditions. Link
implementation PRs there. Consolidate obsolete #40/#48 scope without losing the
recorded experiments or breaking active PR dependencies.

Optimize when a measured release budget fails. A new Chaos solver, distributed
training, all terrain variants, full combat, and a vision policy are not required
for this bounded release. Keep them as separate future proposals.

## Release-ready decision

The release tracking Issue must link evidence for reusable extension, learned
behavior, bounded recovery, independent game integration, packaged execution,
independent developer reproduction, and cleared public delivery. All required
gates must pass and no release-blocking defect may remain.

A successful demo alone cannot establish a general platform. Passing platform
unit tests alone cannot establish a working game or learned controller. The
release requires both, with explicit supported boundaries. Set a version and
date only after the candidate and publication decisions are ready.

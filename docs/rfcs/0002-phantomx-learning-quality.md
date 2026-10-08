# RFC 0002: PhantomX command-tracking objective

Status: proposed, with a Python prototype for review.
Related Issue: [#40](https://github.com/zpyc1oud/EmbodiedUE/issues/40).

## Goal

Train stationary, low-speed and turning behavior with the existing Task pipeline.
Keep the Session timing and exported observation/action contracts unchanged.

## Observed problems

The main-branch command range starts at 0.4 m/s. The curriculum averages error per
transition, although control intervals vary. A fixed 0.2 m/s tolerance can count
standing as successful tracking of a 0.1 m/s command. Turn-stage success ignores
yaw error. The tracking rewards subtract their stationary reference values.

These are training-definition findings. They do not establish which change will
improve a trained policy. The candidate needs real learning and deployment evidence.

## Proposed behavior

- Sample forward speeds from 0.05 to 0.5 m/s.
- Keep a 10 percent standing mixture.
- After turn promotion, use 20 percent of non-standing commands for turning in place.
- Integrate curriculum error and its allowed budget over completed physical time.
- Bound the linear error budget by 0.03 and 0.20 m/s, with half the command speed
  between those limits. Use a 0.25 rad/s mean yaw-error limit in the turn stage.
- Keep termination failures separate from timeouts. Require valid elapsed time.
- Use an unshifted exponential tracking reward for linear velocity and yaw rate.
  Keep the existing progress terms separately configurable.

The command ranges and tolerances are explicit YAML parameters. They are initial
candidate values. Freeze the values used for held-out evaluation and record any
later change as a separate candidate.

## Alternatives

Keeping only the former high-speed range does not cover low-speed deployment.
Keeping a single absolute error threshold permits no-motion behavior at low speed.
A complete generic task rewrite is unnecessary for this focused change.
The broader Isaac Lab comparison remains in Issue #41.

## Run and artifact effects

The new reward objective is `phantomx_tracking_time_v2`. Previous objectives cannot
resume into this objective. Preserve earlier Runs and start a fresh training Run.
No policy warm start or migration layer is added.

The actor input order, action mapping, physical timestep, variable-decimation
contract, and artifact format remain unchanged. This does not make an old policy
qualified for the new behavior requirements.

## Validation

Unit tests use independent 5/35 ms integrals and distinct Slots. They cover sparse
reset, missing valid time, wrong yaw, low-speed no-motion behavior, positive
stationary tracking reward, drift, and turn-in-place commands.

Reward fixtures use documented scalar calculations. Their tolerance stays 1e-6.
Default Python tests, static checks, and documentation checks must pass.
Windows validation must run the affected real Task workflow on the exact candidate.
Evaluate survival and command tracking separately. Record linear and angular
errors, stationary drift, clipping, stopping behavior, seeds and physical duration.

A passing smoke run does not accept this proposal's learning-quality requirement.
Catch, its contact contract, and distribution of the final package remain separate
parts of Issue #40.

## Evaluation criteria

Use `survival_rate` for episodes that reach the time limit without a fall or Slot
fault. Use `tracking_success_rate` for surviving episodes whose physical-time
mean squared normalized errors are below 1 for both linear velocity and yaw.
The linear normalization limit is `clamp(0.5 * command_speed, 0.03, 0.20)` m/s.
The yaw limit is `clamp(0.5 * abs(command_yaw), 0.05, 0.25)` rad/s.
Evaluate zero, low-speed, moving, turning and stopping commands separately.
Freeze these criteria before held-out runs.

Report physical-time linear and yaw RMSE and the Slot-fault rate. Exclude invalid
samples from error averages, but count their episodes as failures. Reset each
Slot's error integrals only when that Slot ends. Record the completed-episode
count with all rates. Zero completed episodes cannot demonstrate behavior.

The player controller must allow yaw input without forward input. S retains its
stop behavior. This makes turn-in-place playback reachable with the same velocity
channel used during training.

# Training benchmark design

Status: proposed protocol, not measured results. Baseline: `42fae6dd404b8a5a74110113cdde251c7ec069b5`.

The maintainer has completed training on one Windows machine. This protocol measures throughput, learning quality, stability, and deployment separately. It supports the [development roadmap](roadmap.md); cross-path behavioral diagnosis follows the [deployment compatibility plan](train-deploy-parity.md).

## Questions the benchmark must answer

1. Which stage limits performance for each Task and Slot count?
2. Does an optimization improve end-to-end training, rather than one microbenchmark?
3. Does faster sampling preserve learning quality and physical semantics?
4. Can another developer reproduce the result on a declared supported setup?
5. Does the trained policy remain usable in the target game?

## G0: correctness before timing

Run the relevant existing test layer before collecting performance evidence. Confirm finite observations/rewards, expected dimensions, Task identity, Slot isolation, initial reset, terminal-state handling, and timeout bootstrap. Validate the intended physics timestep and decimation behavior.

For PhantomX, retain actual-time rewards, discount/GAE scaling, 20-second episode termination, and objective compatibility checks. A changed solver setting or reward definition is a different experiment.

A failed correctness gate invalidates speed claims. Record the failure and repair it; do not remove bad runs as unexplained outliers.

## Measurement tracks

| Track | Work included | Primary result |
|---|---|---|
| B1 environment stepping | Precomputed bounded actions, production Session/DirectEnv step and reset path | Valid transitions/s and stage latency |
| B2 policy rollout | Actual policy inference plus environment sampling, no PPO updates | Rollout throughput and inference/copy cost |
| B3 full PPO | Rollout, update, configured logging and checkpoint work | End-to-end transitions/s, iteration time, update time |
| B4 learning quality | Independent full training runs and fixed evaluation | Return/success versus data and wall time; time to quality |
| B5 stability and deployment | Long training and target-game tests | Faults, resource growth, recovery, behavior transfer |

A simulator-only loop can be an additional diagnostic, but it must not be labeled B1 if it bypasses Task math or reset work. Startup and compilation are measured separately from steady state.

## Counting rules

- A vector step is one accepted batch Step request. It is not one transition per Slot.
- Valid transitions are the sum of valid Slot transitions returned over measured steps. Report attempted transitions and fault/reset counts separately.
- Solver steps are completed physics steps. Report the measured decimation distribution.
- Simulated seconds per Slot are the sum of actual completed control intervals. Aggregate valid simulated time across Slots only under an explicitly named metric.
- Throughput is valid transitions divided by measured wall-clock seconds.
- B3 wall time includes the work enabled by that experiment, including scheduled checkpoint/logging pauses. Also provide decomposed stage time.

With variable decimation, transitions/s alone does not measure simulated-time throughput. Publish both, with the interval distribution. Do not compare runs with different timing as if only the implementation changed.

## Controlled setup

Record CPU model/core count, RAM, GPU/VRAM, driver, Windows build, UE version/build configuration, Python/Torch/CUDA/rsl-rl versions, project commit/local changes, and relevant background load.

Retain resolved Task/Worker/runner settings, Robot asset revision, map, terrain seed, run seed, Slot count, physics timestep, decimation policy, rendering mode, policy architecture, observation normalization, and reward/objective version. Reuse the existing Run manifest and identities rather than creating a second provenance system.

Keep the same inference device, thread settings, logging/checkpoint cadence, precision, solver quality, and reset distribution for an A/B claim. Any intentional difference is a named experimental factor.

Record cold startup/build time and warm startup separately. A dependency sync, asset load, or first CUDA initialization must not disappear into an undocumented warmup.

## Task and scale matrix

Use four workloads:

- W0: CartPole.
- W1: PhantomX flat walking.
- W2: PhantomX continuous terrain.
- W3: PhantomX discrete terrain.

Proposed pilot Slot counts are 16/64/256 for W0 and 16/64/512 for W1. Start W2/W3 at 16/32/64. These are workload levels, not claims that each is feasible or optimal.

If a level exhausts memory or fails initialization, retain that outcome. Choose a documented feasible replacement after the pilot and freeze the final matrix before comparing versions. Do not choose a different favorable scale for each implementation.

The full steady-state matrix is 4 workloads × 3 scales × B1/B2/B3 × 5 independent repeats = 180 runs. A first implementation can use W0/W1 × 3 scales × B1/B3 × 5 repeats = 60 runs. Publish the reduced scope explicitly.

The matrix applies to one fixed configuration comparison point. Additional hardware, rendering variants, or A/B versions multiply the budget. Do not claim 180 runs covers all combinations.

## Warmup and sampling

For B1/B2, warm up for at least 30 seconds and 200 vector steps, satisfying both. For B3, additionally complete at least three PPO updates. Record actual warmup work and exclude it from the steady-state interval.

Measure B1/B2 for at least 120 seconds. Measure B3 for at least 120 seconds and 10 complete updates, satisfying both. Record the final sample duration and work count rather than truncating to a favorable iteration.

Each repeat uses a new process/session and declared seed. Randomize or interleave A/B run order to reduce thermal and background-load effects. Use a predeclared policy checkpoint in B2; different learned policies can change resets and physics load.

Report median and tail step/iteration latency, not just mean throughput. Separate reset-heavy episodes from uninterrupted stepping where useful, while retaining the full production-path headline result.

## Stage timing and instrumentation

Start from existing `StageProfiler` output, including action preprocessing, Step round trip, reward/termination, terminal observations, reset, observation construction, and client encoding/validation/send/wait/decode/device movement.

Add correlation with policy inference, PPO update, UE physics, state collection, and optional recording. Use request/run identifiers already available. Avoid counting nested stage durations twice.

CPU wall clocks measure CPU/IPC elapsed time. GPU execution is asynchronous: use CUDA events for GPU work and synchronize only at defined measurement boundaries. Per-step forced synchronization changes the workload and belongs in a separate diagnostic profile. Follow [PyTorch CUDA timing guidance](https://docs.pytorch.org/docs/stable/notes/cuda.html).

Run an instrumentation-on/off pair on a representative workload. If collection materially changes throughput, publish normal-run results separately from diagnostic traces and report the observed overhead.

## B4: learning quality

Use two pilot training seeds per Task to determine feasible budgets and quality thresholds. Freeze the algorithm/configuration, thresholds, evaluation suite, checkpoint interval, and budget before running five new formal training seeds. Pilot seeds are not formal results.

Use the same transition budget and report simulated time as well. For comparisons that change the control interval, define the scientific question explicitly and match physical-time exposure where required. Show quality against both wall time and sampled data.

Evaluate each policy on 100 episodes using an independent, fixed evaluation seed set and fixed commands/terrains. Disable exploration for deterministic-policy evaluation and freeze normalization updates. Separate in-distribution evaluation from held-out maps. Do not restore training curriculum automatically into the test distribution.

Initial candidate quality thresholds:

- CartPole: at least 95 of 100 episodes survive the 300-control-step horizon.
- PhantomX flat: at least 95% survive 20 simulated seconds, plus a frozen command-tracking criterion.
- PhantomX terrain: at least 90% survive 20 simulated seconds, plus a frozen command-tracking criterion.

These are proposed starting points. Survival alone cannot certify locomotion: a standing policy may survive while ignoring the command. Define and freeze tracking thresholds after the pilot; report tracking error, progress, non-foot contact, and action saturation as separate metrics.

Time to quality is the wall time to the first of two consecutive scheduled evaluations meeting the frozen criterion. Report evaluation cadence and whether evaluation time is included. A run that never meets the criterion is right-censored at its budget, not discarded or replaced by the best seed.

## B5: stability and deployment

For a release candidate, propose an eight-hour full-training run on the supported reference setup. Record throughput over time, memory/VRAM, checkpoint completion, invalid states, Slot faults, Session failures, and resource cleanup. An eight-hour pass is bounded evidence, not proof of unlimited stability.

Test normal stop, interruption, checkpoint continuation, and process cleanup through existing lifecycle seams. Test recoverable Slot faults separately from Session-fatal faults. An ambiguous transport failure must not replay a Step.

For deployment, execute the four layers in the [compatibility plan](train-deploy-parity.md): fixed inputs, fixed actions, same-scene closed loop, then target maps/operating variation. Include actual render/solver timing, query filtering, overruns, stale commands, and the host's tested response. Do not equate successful ONNX loading with successful deployment.

## Raw data and result structure

Keep these logical records within the existing run/artifact layout:

- Run metadata: workload, track, hardware/software, configuration, seeds, version, start/end, outcome, and failure reason.
- Measurement intervals: timestamps, vector steps, valid/attempted transitions, solver steps, simulated time, stage durations, reset/fault counts, memory/VRAM.
- Evaluation episodes: training seed, evaluation seed, map/command case, simulated duration, survival/success, return, tracking metrics, and termination reason.
- Summary: aggregation method, independent sample count, uncertainty, exclusions with reasons, and links to underlying records.

Use structured JSON/JSONL or CSV for values and a short Markdown report for interpretation. Preserve unit names. Raw per-step tracing is optional; bounded aggregate records should remain inexpensive.

## Analysis and regression decisions

Compute summaries per independent repeat, then compare repeats. For paired A/B runs, report paired changes at the same workload/scale/seed. Use confidence intervals over independent runs or training seeds. Steps within a run and evaluation episodes from one policy are not independent training runs.

Report throughput together with memory, quality, and failures. State the supported workload for each conclusion. A lower solver quality or fewer observations is a changed configuration, not an unqualified optimization.

Establish regression margins from baseline variability and user impact before final A/B collection. Do not invent a universal 5% gate without a baseline. A confidence interval that includes both useful improvement and harmful regression is inconclusive; publish that result rather than selecting the best repeat.

## Isaac Lab comparisons

Use [Isaac Lab's benchmark documentation](https://isaac-sim.github.io/IsaacLab/main/source/overview/reinforcement-learning/performance_benchmarks.html) to understand its measurement conditions, not to import headline numbers.

A controlled comparison needs equivalent Robot/Task definitions, timing, observations, rendering, devices, workload size, learning budget, and quality criteria. If those conditions cannot be matched, present separate system profiles and explain the differences. Do not rank systems using cross-hardware FPS alone or imply PhysX and Chaos dynamics are identical.

## Implementation order and limitations

1. Implement raw interval records and an aggregation report using existing profiling.
2. Run G0 and the 60-run reduced matrix.
3. Investigate the measured bottleneck with a scoped change.
4. Run a paired A/B comparison and B4 quality checks.
5. Expand to the full matrix and B5 for the declared release scope.

This protocol has not been executed as part of the documentation change. Hardware access, feasible scale, learning budgets, tracking thresholds, and regression margins remain to be established through the specified pilots. Runtime correctness still requires the real-UE layers in the [test guide](../../tests/README.md).

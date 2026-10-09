# Measure runtime cost without training

Use this benchmark to inspect the existing Worker and DirectEnv step path.
It sends zero policy actions. It does not create a policy network, run PPO, tune
parameters, or measure learned behavior. Curriculum adaptation and training
events are not installed.

## Run a bounded case

Build the matching UE host first. Select the same source revision for Python and
UE. Use a new output path for each case:

```powershell
uv run python scripts/benchmark_runtime.py --task UERL-PhantomX-Walk-v0 --num-envs 1 --decimation 4 --warmup-steps 20 --steps 200 --output artifacts/runtime/phantomx-1.yaml
```

The usual `--host-profile`, `--ue-executable` and `--project` options select the
host. `--map` overrides the Task map. Omit `--decimation` to retain the Task's
configured interval range. The report records the effective choice.

The script creates a sibling Run directory for Session evidence. It refuses an
existing report or Run path. It writes a YAML result on success or runtime
failure and closes only the Session that it owns. Invalid or faulted Slots
abort the case. Ordinary Task episode termination is counted separately.

The profiler returns stage metrics in memory. The benchmark does not create a
new client stage JSONL stream. Existing Worker/Session logs retain their normal
formats in the Run directory.

## Read the report

- `initialization_seconds` covers Task construction and Session initialization.
- `warmup_steps` are excluded from measured latency and physical time.
- `wall_seconds` covers the measured loop, including its validity/accounting work.
- `step_latency_s` gives mean, median and linearly interpolated p95 latency for
  each step and its output-validity checks.
- `control_steps_per_wall_second` counts synchronized control windows.
- `slot_transitions_per_wall_second` multiplies control windows by the Slot count.
- `physical_seconds_per_slot` sums actual completed intervals.
- `physical_seconds_per_wall_second` is the per-Slot simulation-time ratio.
- `shutdown` and `shutdown_seconds` report Session cleanup separately.

Client detail stages are inside `step_roundtrip`. Do not add them to the parent
stage when calculating total time. These are host wall-clock measurements,
not GPU kernel timings. Do not compare throughput figures with different
Slot counts, timing ranges, scenes or measured horizons without recording those
differences.

## Check memory and repeatability

Start with one Slot. If resources permit, run separate 16-Slot and 64-Slot cases
with the same task, decimation, warmup and measured steps. Repeat each selected
case three times with new output names. Compare spread as well as the mean.

On Windows, record system committed bytes and commit limit before, during and
after each case. Record private bytes for the exact Python and UE PIDs printed
by the script. Record GPU memory separately if it is relevant. Available RAM
alone does not establish commit capacity. System commit includes other programs;
do not attribute all of it to this benchmark.

Do not run an additional model-loading analysis process during a memory-limited
case. Do not start concurrent UE instances, change pagefile settings, or close
unrelated programs to improve the reported result. Stop the owned case if
allocation fails or commit usage continues toward the limit. Preserve the
partial report and logs.

The first supported parallel count is the largest measured count with sufficient
host headroom and stable repeated timing. It is not a universal product limit.
A small smoke training/export check can validate its separate path later; a long
learning run is not required to collect this runtime baseline.

## Compare an optimization

Pin the baseline and candidate commits. Rebuild UE when native source changes.
Use the same host, Task, map, seed, Slot counts, decimation and measurement horizon.
Keep the raw reports from both revisions. Run the short cases three times per
revision, then run at least 2200 measured steps at D4 for the 20-second PhantomX
Task. Record the completed episode count and Worker resets.

Compare control-step latency, Slot throughput, stage cost and process memory.
Report the individual runs and their spread. A mean of per-run p95 values is not
a pooled p95. Separate Python private bytes from UE private bytes and system
commit. Configuration-only import savings do not establish lower learner memory.

Before accepting a speedup, run the affected native and real UE checks. Replay
the same bounded action sequence at both revisions. Compare physical state,
observations, fault handling and reset behavior with the existing tolerances.
Do not change solver settings or reduce sensing to improve this comparison.

## Inspect the physics phase

The Worker `physics_frame` timer spans its PrePhysics and PostPhysics hooks.
It includes the solver task wait and game-thread result synchronization.
Use a bounded CPU trace to separate these costs before changing the solver.
[Unreal Insights](https://dev.epicgames.com/documentation/unreal-engine/using-the-timers-and-counters-tabs-in-unreal-insights-for-unreal-engine)
can export timer statistics for a selected time interval without opening its UI.
Exclude initialization and warmup. Do not add parent/child inclusive timers or
concurrent worker-thread time to the game-thread wait. Measure throughput again
without tracing when comparing revisions.

A headless Worker uses Scene-local synchronous outer solver dispatch. It calls
the same Chaos advancement code and retains the normal per-frame result sync.
The scheduling scope restores the previous mode when the Worker deactivates.
It does not change the buffer mode, fixed dt, substep configuration, solver
iterations or collision settings. Rendered Workers and game deployment retain
their existing scheduling. A changed dispatch order still requires trajectory,
reset and lifecycle validation on the matching UE build.

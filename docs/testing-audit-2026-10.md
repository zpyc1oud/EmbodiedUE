# Repository Testing Audit (2026-10)

## Scope and method

This audit describes the repository at `b6f2f75b8be9bcc1251e5aa9d4aaf885d206b88d`.
Counts are source inventory counts, not expanded pytest parameter-case totals:
Python cases count test functions, and UE cases count
`IMPLEMENT_SIMPLE_AUTOMATION_TEST` declarations. The inventory was generated
from the checked-in test paths and compared with the runners, pytest
configuration, and contribution documentation.

## Inventory

| Layer | Test files | Named cases | What it covers |
|---|---:|---:|---|
| Python unit | 74 | 528 | Task math, configuration, CLI, policy artifacts, training and lifecycle modules |
| Python integration | 9 | 45 | Session boundaries, saved-run configuration, resume, and Direct/Manager composition |
| Protocol unit and integration | 2 | 8 | Wire codec and socket client behavior against test-only support code |
| Parity | 7 | 17 | Python/UE plan, artifact, action, and deployment cases with reviewed fixtures |
| Tooling | 7 | 36 | Package generation, installation, dependency boundaries, runners, and profiling |
| Real UE E2E | 7 | 20 | Unreal process, Worker lifecycle, DirectEnv, and terrain scenarios |
| UE Automation | 31 C++ files | 149 | Native module tests for interface, transport, policy, robot, terrain, and Worker behavior |

`pyproject.toml` collects Python unit, Python integration, protocol, tooling, and
parity tests by default. Real UE E2E tests are intentionally excluded and are
run through `scripts/run_e2e.py`, which fails early when its UE executable or
project is absent. `scripts/run_all_tests.py` runs the default pytest collection,
two filtered UE Automation groups, and the real UE E2E suite. The test guide
states that mocks do not verify Chaos or other engine behavior.

## Findings

1. **The test layers are already explicit and broad.** Python behavioral tests,
   reviewed parity fixtures, native UE Automation, and real UE E2E have separate
   homes and commands. The existing guide clearly distinguishes mock evidence
   from engine evidence. There is no evidence for replacing these layers or
   mechanically rewriting the existing suite.
2. **The audited GitHub workflow did not run pytest.**
   `.github/workflows/static-checks.yml` ran Ruff and Mypy at the inventory
   baseline. Local contribution guidance asks for `pytest -q`, and the full
   validation runner starts with that same suite. This left ordinary Python
   behavior outside the automated PR gate. This task adds the configured
   default pytest suite to CI after confirming it passes locally; its pytest
   collection deliberately excludes UE-only E2E and runs without UE.
3. **The native UE suite needs a Windows/UE host.** The current environment is
   Linux and does not provide the project's UE 5.8 installation, so UE
   Automation, Chaos, and real Worker E2E remain unverified here. Keep that
   status separate from Python and static-check results.
4. **The external Direct package path is narrower than the public hooks.** The
   existing Direct/Manager equivalence coverage exercises an external-entry
   point fixture, while `uerl new external-cartpole` produces a Manager package.
   An installed generated Direct package should be covered through package
   discovery and the public CLI/configuration path; matching response lengths
   or reading generated source alone is insufficient.
5. **UE Automation completion thresholds are smoke checks, not case identity
   proofs.** `run_all_tests.py` checks successful log completion and minimum
   counts for each filter. The filters are currently explicit, but threshold
   counts alone would not prove that every expected case ran if a filter or
   test name drifted. The contribution workflow already tracks suite selection
   and completion accountability in Issue #13, so this audit records the risk
   without duplicating that runner work.

## Isaac Lab comparison

The comparison uses the official Isaac Lab `v2.3.2` release
(`37ddf626871758333d6ed89cf64ad702aef127d0`), which is listed as compatible
with Isaac Sim 4.5/5.0/5.1. That release is closer to the current Isaac Sim 5.x
testing model than its moving `main`, which now targets Isaac Sim 6.1.

The official contribution guide calls for pytest coverage of normal and edge
behavior and provides full-suite, file, and individual-test commands. Its
`run_all_tests.py` runs tests in isolated processes with test timeouts; the
source tree has CPU-friendly unit cases as well as tests that launch Isaac Sim.
For example, the termination-manager tests use a small dummy environment and
assert the changing termination and timeout masks. Task environment smoke tests
instantiate registered tasks, reset them, apply batched actions, and check
returned tensors; those are launch/shape/finite-value smoke checks rather than
independent physics or reward oracles.

The official Direct workflow inherits `DirectRLEnv` and separates environment
configuration from the environment class. Its template tooling creates
installable projects and registers generated tasks. This is useful precedent
for packaging, registration, and generated-project validation, but it does not
provide an engine-independent Python-hooks contract. Isaac Lab's `env_ids` are
valid vectorized environment indices; they do not define EmbodiedUE's invalid
Worker Slot behavior. Likewise, Isaac Sim test fixtures cannot stand in for UE
Automation or prove UE slot routing.

The current official Isaac Lab PR workflow also runs pytest suites in its GPU
Docker environment and stores JUnit reports. EmbodiedUE's pure Python suite can
use the same general practice in its existing Ubuntu workflow without adding UE
to CI. The external task installer and capability preflight remain EmbodiedUE
responsibilities; they need package-discovery, CLI-boundary, and independent
behavior tests.

## Source references

- [Isaac Lab v2.3.2 contribution and unit testing guide](https://isaac-sim.github.io/IsaacLab/v2.3.2/source/refs/contributing.html)
- [Isaac Lab v2.3.2 Direct environment tutorial](https://isaac-sim.github.io/IsaacLab/v2.3.2/source/tutorials/03_envs/create_direct_rl_env.html)
- [Isaac Lab v2.3.2 project/task template guide](https://isaac-sim.github.io/IsaacLab/v2.3.2/source/overview/own-project/template.html)
- [Isaac Lab v2.3.2 test runner](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/tools/run_all_tests.py)
- [Isaac Lab v2.3.2 termination manager tests](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/source/isaaclab/test/managers/test_termination_manager.py)
- [Isaac Lab v2.3.2 task smoke tests](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/source/isaaclab_tasks/test/test_environments.py)
- [Isaac Lab main at `b0542fe`: Build and Test workflow](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/.github/workflows/build.yml)
- [Isaac Lab main at `b0542fe`: pytest runner action](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/.github/actions/run-tests/action.yml)

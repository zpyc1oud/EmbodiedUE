# Repository Testing Audit (2026-10)

## Scope and method

This audit describes revision `b6f2f75b8be9bcc1251e5aa9d4aaf885d206b88d`.
Its counts describe source inventory, not expanded pytest parameter cases.
Python counts refer to test functions.
UE counts refer to `IMPLEMENT_SIMPLE_AUTOMATION_TEST` declarations.

The audit compared checked-in test paths with runners, pytest configuration, and contribution documentation.
The inventory remains tied to that revision.
It is not a current execution result.

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

Default `pyproject.toml` collection includes Python unit, Python integration, protocol, tooling, and parity tests.
It excludes real UE E2E.
The command `scripts/run_e2e.py` runs those cases and fails early without the UE executable or project.
The full runner, `scripts/run_all_tests.py`, adds two filtered UE Automation groups and real UE E2E to default pytest collection.
The test guide separates mock evidence from Chaos and engine evidence.

## Findings

1. **Explicit test layers already exist.**
   Python behavior cases, reviewed parity data, native Automation, and real E2E have separate locations and commands.
   The guide separates mock and engine evidence.
   The audit found no reason to replace these layers or mechanically rewrite the complete suite.
2. **CI has a deliberate static-only scope.**
   The workflow `.github/workflows/static-checks.yml` runs Ruff and Mypy.
   The user selected this scope.
   The absence of pytest is a recorded boundary, not authorization to expand CI.
   Changes still require the contributor workflow's Python validation.
3. **Native UE validation needs a Windows/UE host.**
   This audit establishes no UE Automation, Chaos, or real Worker E2E execution result.
   Run those cases on the required Windows/UE 5.8 host.
   Keep their evidence separate from Python and static checks.
4. **The generated external Direct path has a coverage gap at the audited revision.**
   Existing Direct/Manager equivalence cases use an external-entry fixture.
   The command `uerl new external-cartpole` generates a Manager package.
   Public Direct hooks lack equivalent generated-package installation, discovery, CLI, and configuration coverage.
   Add independent numerical assertions.
   Response lengths or generated-source inspection alone are insufficient.
5. **Completion thresholds do not identify completed cases.**
   The full runner examines successful completion logs and minimum counts per filter.
   Explicit filters are useful, but count thresholds cannot prove complete coverage after a name or filter changes.
   Issue #13 has the closed reason `not_planned` and remains closed.
   Issue #19 tracks this layered audit and later verification.

## Isaac Lab comparison

The comparison uses official Isaac Lab `v2.3.2`, revision `37ddf626871758333d6ed89cf64ad702aef127d0`.
Its listed Isaac Sim compatibility is 4.5/5.0/5.1.
The references below pin the upstream revisions.

The contribution guide requires pytest coverage for normal and edge behavior.
It gives suite, file, and individual-case commands.
Its `run_all_tests.py` uses isolated processes and timeouts.
The source has CPU-friendly unit cases and cases that launch Isaac Sim.

Termination-manager cases use a small dummy environment with changing termination and timeout masks.
Task smoke cases instantiate registered Tasks, reset them, apply batched actions, and inspect output tensors.
Those cases establish launch, shape, and finite-value behavior.
They do not supply independent physics or reward oracles.

The Direct entry point inherits `DirectRLEnv` and separates configuration from the environment class.
Template tools generate installable projects with Task registration.
These patterns inform package and registration validation.
They do not define an engine-independent Python-hooks contract.

Isaac Lab `env_ids` refer to valid vectorized environment indices.
They do not define invalid Worker Slot behavior in EmbodiedUE.
Isaac Sim fixtures cannot replace UE Automation or prove UE Slot routing.

The separately referenced Isaac Lab PR workflow runs pytest in a GPU Docker environment and saves JUnit reports.
That comparison does not authorize changes to EmbodiedUE's selected static-only CI scope.
External-package installation and capability preflight remain EmbodiedUE responsibilities.
They require discovery, CLI-boundary, and independent behavior tests.

## Layer-by-layer comparison

| EmbodiedUE layer | Isaac Lab v2.3.2 evidence | Review and decision |
|---|---|---|
| Python unit | `source/isaaclab/test/managers/test_termination_manager.py` uses a small dummy environment and changing expected masks. | Same useful pattern: CPU-level tests with fixed numeric expectations. Keep existing task math tests; no suite-wide rewrite. |
| Python integration | `source/isaaclab_tasks/test/test_environments.py` and `env_test_utils.py` launch registered tasks, reset them, send batched actions, and check outputs. | These smoke checks do not prove reward math or UE lifecycle. EmbodiedUE retains installed entry-point and scripted-session checks as separate layers. |
| Protocol | The tagged Isaac Lab test tree has no U4/U5 socket/wire codec counterpart. | The independent protocol oracle and socket-fragmentation cases are specific to EmbodiedUE's wire contract; keep them project-owned. |
| Parity | `source/isaaclab_tasks/test/test_environment_determinism.py` compares repeat runs in one Isaac Lab simulator setup. | Same-simulator determinism is not cross-language parity. Keep reviewed Python/UE fixtures and explicit numeric tolerances. |
| Tooling/package | Isaac Lab's `tools/template/generator.py` and template guide create external projects; the tagged `tools/template` tree has no pytest install/discovery suite. | EmbodiedUE's real wheel install, entry-point discovery, resource loading, and CLI checks cover an extra contract that the upstream template leaves manual. |
| Real UE E2E | Isaac Lab's pytest cases use its Isaac Sim/Kit launcher and simulator-owned lifecycle. | They cannot validate Unreal Worker startup, Chaos, Slot routing, or UE process ownership. Keep EmbodiedUE's separate UE E2E and mark it blocked without a Windows/UE host. |
| UE Automation | No Unreal C++ Automation test layer exists in the Isaac Lab release. | This is stack-specific and has no transferable test harness. Keep the native C++ cases in their UBT modules; do not infer execution from Python results. |

## Source references

- [Isaac Lab v2.3.2 contribution and unit testing guide](https://isaac-sim.github.io/IsaacLab/v2.3.2/source/refs/contributing.html)
- [Isaac Lab v2.3.2 Direct environment tutorial](https://isaac-sim.github.io/IsaacLab/v2.3.2/source/tutorials/03_envs/create_direct_rl_env.html)
- [Isaac Lab v2.3.2 project/task template guide](https://isaac-sim.github.io/IsaacLab/v2.3.2/source/overview/own-project/template.html)
- [Isaac Lab v2.3.2 test runner](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/tools/run_all_tests.py)
- [Isaac Lab v2.3.2 termination manager tests](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/source/isaaclab/test/managers/test_termination_manager.py)
- [Isaac Lab v2.3.2 task smoke tests](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/source/isaaclab_tasks/test/test_environments.py)
- [Isaac Lab v2.3.2 environment determinism tests](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/source/isaaclab_tasks/test/test_environment_determinism.py)
- [Isaac Lab v2.3.2 task smoke helpers](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/source/isaaclab_tasks/test/env_test_utils.py)
- [Isaac Lab v2.3.2 template generator](https://github.com/isaac-sim/IsaacLab/blob/37ddf626871758333d6ed89cf64ad702aef127d0/tools/template/generator.py)
- [Isaac Lab main at `b0542fe`: Build and Test workflow](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/.github/workflows/build.yml)
- [Isaac Lab main at `b0542fe`: pytest runner action](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/.github/actions/run-tests/action.yml)

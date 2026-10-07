# Write tests

Use this guide when you add behavior or repair a defect in EmbodiedUE.
The [test guide](../../tests/README.md) gives the suite layout and execution commands.
The [contribution workflow](../contribution-workflow.md) gives the review and merge requirements.

## Before you start

1. Identify the behavior that changed.
2. Read the applicable contract in [CONTEXT](../../CONTEXT.md), the accepted RFC, or the module documentation.
3. Find the nearest existing test.
4. Write down the input, operation, and expected result.
5. Select a test layer from the next section.

The expected result must come from the contract, an independent calculation, or reviewed reference data.
Do not calculate it with the function under examination.
A shape assertion proves a shape requirement.
It does not prove a correct reward, physical response, or reset.

## Select the test layer

| Layer | Location | Use |
|---|---|---|
| Python unit | `tests/python/unit/` | Mathematics, configuration, masks, and components without an external process |
| Python integration | `tests/python/integration/` | Session contracts and component interaction through a controlled dependency |
| Protocol | `tests/protocol/` | Frame bytes, strict encoding, layouts, and socket behavior |
| Parity | `tests/parity/` | Python and C++ results against reviewed reference values |
| Tooling | `tests/tooling/` | Package installation, discovery, resources, generators, and scripts |
| UE Automation | Module `Private/Tests/Unit/` directories | Native code and supported engine behavior |
| Real UE E2E | `tests/e2e/` | Actual Worker processes, Chaos, reset, presentation, and process ownership |

Choose the smallest layer that can detect the defect.
Add another layer when the change crosses a contract.
For example, a new plan operator needs Python, C++, parity, and applicable engine evidence.
Obey the [operator admission procedure](../../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md).

Native C++ files belong in their UBT module.
The directory `tests/ue/unit/` contains an index, not a separate C++ build target.

## Write a Python unit test

Use the existing file when it owns the behavior.
Use a new `test_*.py` file for a separate component.
Name each function after the behavior and its condition.
Keep an existing acceptance-case identifier when that suite uses one.

Separate these parts:

- Arrange: create fixed inputs and the object under examination.
- Act: call the public operation.
- Assert: compare observable results with independent expected values.

### Example: reward calculation

Read [test_reward_manager.py](../../tests/python/unit/test_reward_manager.py).
The function `test_ac_py_unit_rewmgr_001_weighted_sum_with_negative_weight` uses three fixed terms:

| Term | Value | Weight | Contribution |
|---|---:|---:|---:|
| track | 1.0 | 2.0 | 2.0 |
| penalty | 1.0 | -0.5 | -0.5 |
| bonus | 0.25 | 4.0 | 1.0 |

The expected total is 2.5 for each of two rows.
The existing test contains this assertion:

```python
assert torch.allclose(total, torch.full((2,), 2.5))
```

This is an excerpt, not a complete test file.
Use the imports and fixtures in the linked file.
Do not replace the constant with another call to `RewardManager.compute`.

For time-dependent rewards, use both short and long control intervals.
The existing rate-reward case uses 5 ms and 35 ms intervals.
Its expected totals are -2.5 and 0.5.
The discrete fall cost occurs once in each interval.

### Example: termination masks

Read [test_termination_manager.py](../../tests/python/unit/test_termination_manager.py).
The first case has three rows with different termination conditions.
It asserts the `terminated`, `truncated`, and reason masks separately.

Use distinct row values.
Identical rows can hide an index error.
For sparse operations, assert the stable Slot IDs and the unchanged Slots.
Do not confuse a compact batch row with its original Slot ID.

## Control fixtures and state

A fixture supplies test data, objects, or resources with a defined lifetime.

Use `tmp_path` for temporary files.
Use a separate output directory for each invocation.
Do not write test output into checked-in assets or an existing Run.

Use `monkeypatch` for temporary environment variables and dependency replacement.
Restore process-wide state after the case.
Prefer function-scoped mutable fixtures.
Use broader fixture scope only when the resource supports safe reuse.

Set the relevant seed explicitly when random behavior affects the assertion.
Record the seed for a failed randomized case.
Do not assume that one seed makes two different engines numerically identical.

For reset behavior, distinguish three states:

1. The input state used for the action.
2. The transition state before reset.
3. The state returned after reset.

Assert terminal values before reset replaces them.
Assert that reset affects only the selected Slots.
Include repeated reset and failure cleanup when the changed contract permits those operations.

## Write an integration test

Read [test_session.py](../../tests/python/integration/test_session.py).
Its `_FakeBridge` supplies a controlled Session dependency through `bridge_factory`.
The real Session code still performs the operation.

Replace the external dependency, not the logic under examination.
For example, a Session test can record Bridge requests and supply known responses.
It must not replace the Session method whose behavior it claims to prove.

Assert call order only when the order is part of the public contract.
Manifest creation before Ready is such a contract.
An internal helper call is not sufficient evidence of correct behavior.

Include the relevant failure path:

- A dependency fails before initialization.
- Initialization stops after it acquires a resource.
- A request times out.
- A connection closes during an operation.
- Cleanup occurs after an exception.

Assert the resulting state and the owned resource release.
Do not only assert that an exception occurred.

## Write protocol and parity cases

Read [test_bridge_codec.py](../../tests/protocol/unit/test_bridge_codec.py).
Its cross-language header case uses literal expected bytes.
Keep the reference independent of the encoder under examination.

Include malformed data that the supported interface can receive.
Examples include duplicate keys, non-finite values, unsafe integers, and fragmented input.
Do not add unrelated defensive cases without a reachable project requirement.

The [socket support tests](../../tests/protocol/integration/test_socket_bridge_client.py) exercise the test-support client.
They do not, by themselves, establish correct product-client behavior.

For an operator, add reviewed data under `tests/parity/cases/<operator>/`.
Use [test_operator_parity.py](../../tests/parity/test_operator_parity.py) as the Python entry point.
Both implementations must match the reviewed expected result.
Agreement between two implementations can preserve the same defect.

Keep protocol and parity fixture formats unchanged.
The use of YAML for project configuration does not change these JSON contracts.

State the reason for each numerical tolerance.
Use exact equality for discrete values where the contract requires it.
Do not increase a tolerance to hide a failed assertion.
Deployment inference fixtures use their specified tolerances, including `atol=rtol=2e-5`.
Do not apply that limit to an entire physical rollout without a separate basis.

## Write package and generator tests

Read [test_external_task_package.py](../../tests/tooling/test_external_task_package.py).
Build the actual package.
Install it into an isolated target.
Run the probe outside the source directory.

Make sure that discovery does not depend on the source checkout.
Make sure that resources remain accessible after removal of staged source files.
Make sure that Task construction and CLI configuration meet the contract.
Assert that package import does not change built-in defaults.

For generator changes, use [test_cli_new.py](../../tests/python/unit/test_cli_new.py).
Include the generated result in the relevant package test.
A source-text assertion cannot replace an installation test.

## Write UE Automation cases

Read [UERLActuatorLawTests.cpp](../../engine/Plugins/UERLEngine/Source/UERLRobot/Private/Tests/Unit/UERLActuatorLawTests.cpp).
It uses `IMPLEMENT_SIMPLE_AUTOMATION_TEST` inside `WITH_DEV_AUTOMATION_TESTS`.
Its position-PD case has an independent expected effort of 6.5.

Use the module's existing test flags and name prefix.
Build the Editor target after C++ changes.
Run the exact changed case before its applicable group.

For a physical requirement, use the actual engine object and completed solver state.
A pure arithmetic case does not establish correct Chaos integration.
For lifecycle changes, include start, stop, repeated use, reset, and applicable callback interruption.

Keep the assertion output and completed case names.
A success count alone cannot identify which cases ran.
`[VERIFY]` messages supplement assertions.
They do not replace them.

## Write real UE E2E cases

Start with [test_cartpole_worker.py](../../tests/e2e/test_cartpole_worker.py)
and [test_p2_direct_env.py](../../tests/e2e/test_p2_direct_env.py).
Use the existing process and Session helpers.

Give the case a bounded timeout.
Keep enough output to identify the failing stage.
In cleanup, release only processes and files owned by that invocation.
Do not terminate an unrelated Editor or all processes with the same executable name.

Assert the changed physical or lifecycle effect.
Where isolation matters, also assert an unaffected Slot.
For a timeout, distinguish failure before Worker startup from a failed physical assertion.

Use actual authorized assets for a content-dependent case.
A Git LFS pointer is not a usable asset.
Record missing content as a blocker.
Do not substitute another scene and report the original requirement as passed.

## Run the checks

Run commands from the repository root.

1. Install the locked development dependencies.

   ```powershell
   uv sync --locked --group dev
   ```

2. Run the affected case.

   ```powershell
   uv run pytest tests/python/unit/test_reward_manager.py::test_ac_py_unit_rewmgr_001_weighted_sum_with_negative_weight -q
   ```

   The selected case must complete and pass.
   A collection result with zero cases is not a pass.

3. Run the affected file.

   ```powershell
   uv run pytest tests/python/unit/test_reward_manager.py -q
   ```

4. Run the checks required by the change.

   ```powershell
   uv run pytest -q
   uv run ruff check src scripts tests
   uv run mypy src tests
   ```

Default pytest collection excludes real UE E2E.
GitHub CI currently runs Ruff and Mypy only.
A green static check is not runtime evidence.

On a built Windows/UE host, select the applicable engine scope:

```powershell
uv run python scripts/run_e2e.py --suite ue
uv run python scripts/run_e2e.py --suite p2
uv run python scripts/run_e2e.py --suite all
uv run python scripts/run_all_tests.py
```

Use only the commands needed for the selected scope.
The full runner already includes Python, UE Automation, and E2E.
Do not repeat an unchanged successful suite without a specific reason.

For custom UE paths, set `UE_ROOT` or `UE_58_ROOT`.
The E2E Worker runner selects `UE_ROOT` first.
It then selects `UE_58_ROOT`, or the default Epic installation.
See the [test guide](../../tests/README.md) for native filters and commands.

## Prove a regression repair

1. Add a case that reproduces the actual defect.
2. Run it against the defective implementation, where practical.
3. Make sure that the intended assertion fails.
4. Apply the repair.
5. Run the case again.
6. Run the affected aggregate checks.

An import error is not proof that a behavioral regression test works.
If the earlier revision cannot run, record that limitation.
Do not weaken the assertion to obtain a pass.

## Review the evidence

Before review, answer these questions:

- Does each assertion establish the changed contract?
- Are the expected values independent?
- Are fixtures and temporary resources isolated?
- Are failure paths and cleanup covered where relevant?
- Are reset values and Slot IDs distinct?
- Does the evidence require actual UE execution?
- Do the selected and completed case names agree?
- Are the tolerances justified?
- Do the commands use the tested revision?
- Are unexecuted checks identified?

Keep the commit, any dirty diff, command, tool versions, seed, and report location.
Use the workflow result states: Passed, Failed, Skipped, or Blocked.
A required skipped or blocked case prevents a full-pass claim.

Keep startup smoke, numerical parity, physical response, and learning quality separate.
One training iteration establishes that a path can execute.
It does not establish policy convergence or reliable scene behavior.

## Test documentation checks

Documentation tooling cases belong in `tests/tooling/test_documentation_checks.py`.
Use temporary Markdown files with one intended defect.
For a missing link or heading, assert the source line and destination in the diagnostic.
Include a valid link to show that the check distinguishes the two cases.
For CLI examples, test an unknown flag, a missing required argument, and a valid command.
Use the product parser without calling the command implementation.
A documentation check must not launch training, UE, or a shell command from an example.

Run the repository-wide check before you submit documentation changes:

```powershell
uv run python scripts/check_docs.py
```

See [the documentation check](../../CONTRIBUTING.md#check-documentation) for supported syntax and manual review requirements.

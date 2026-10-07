# Test-quality review

[Issue #29](https://github.com/zpyc1oud/EmbodiedUE/issues/29) owns the repository-wide assertion review.
[The YAML record](issue29-review.yaml) accounts for 236 tracked test and support files at its recorded baseline.
It includes the external example test and tooling/CI configuration missing from the earlier 231-file inventory.

Nine files have full-file review decisions in this CPU slice.
Seven retain focused historical checks; their remaining assertions still need review.
There are 227 files awaiting full-file review, including those seven.
A passing suite does not complete that review.

The CPU changes retain established numerical tests, remove a redundant local-dictionary identity assertion,
and strengthen saved-artifact binding, distinct reward/action rows and mixed terminal/timeout PPO returns.
Four temporary faults in an isolated copy fail the intended assertion; restored tests pass.
They reverse saved observation members, broadcast the first reward row, omit action clipping,
and use a fixed reference discount for timeout bootstrap.
These demonstrate detection, not four new product defects.

See [Write tests](../how-to/write-tests.md) for the implemented examples and [Tests](../../tests/README.md) for commands.
Run applicable native and real workflow checks when later slices change those contracts.
Keep physical response and learning quality separate from this CPU evidence.

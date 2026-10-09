# RFC 0002 Shared input provider contract

- Status: Proposed
- Created: 2026-10-09
- Baseline: `0aeea37f8cc0da0cd71a43d0ea5bb61f6a290841`
- Tracking: [Issue #52](https://github.com/zpyc1oud/EmbodiedUE/issues/52)
- Detailed design: [Platform extension architecture](../design/platform-extension-architecture.md)

## Problem

Robot and Task composition already have reusable interfaces. Native observation
selection still uses a closed quantity enum and fixed Robot selector grammar.
Ground sampling shares code, but training/deployment query geometry and filtering
can differ. Adding a provider should not require Task-specific runtime dispatch
or a separate game-only observation implementation.

## Decision proposed

Introduce a stable native input factory registry and a compiled per-batch input
set shared by Worker and game deployment. Keep existing field metadata and plan
execution. Separate immutable input specifications from concrete scene bindings.
Use synchronous float32 numeric providers first.

Factories validate configuration and output descriptors. Instances bind once,
sample at the host's completed-window boundary, reset selected Slots, and release
owned resources. Repeated reads cannot resample a boundary or advance time.
The Robot runtime remains the owner of physical bodies and actuators.

Extract built-in state and ray-ground sampling behind this seam before adding an
external provider example. Preserve initial numeric behavior in extraction PRs.
Treat any change in ray geometry, filtering or cached-hit policy as a separate
semantic change with its own validation and training consequences.

## Isaac Lab basis and UE adaptation

The detailed design pins official Isaac Lab sources for scene composition,
entity resolution, sensor configuration/lifecycle, ray patterns, and Direct/Manager
Tasks. Reuse the separation of configuration, binding, sampling and Task terms.
UE deployment requires native provider identities and serialized requirements
because no Python simulator process is present in the game.

Do not copy USD scene ownership, assume a GPU physics backend, or infer support
for arbitrary image inputs from a generic interface. Keep the accepted Session
clock, sparse reset and shared observation/action plans.

## Public and persisted contract changes

The input specification includes provider identity/version, effective parameters,
logical attachment/scene binding, output descriptors and sampling/failure semantics.
The Run stores this semantic configuration. The artifact carries the requirements
needed to reject an incompatible game integration before enabling the policy.

The current artifact format version is 1. Mandatory input metadata requires a
new version and coordinated Python/C++ readers. The exact schema and protocol
version decision must be approved before implementation. Do not silently infer
missing provider semantics from a historical artifact or Task name.

Preserve historical Runs and artifacts with their matching runtime. Re-export
only when the exact original contract is available and unchanged. A materially
changed input contract may require fresh training. No general legacy migration
or compatibility layer is proposed.

## Alternatives

1. Extend the closed observation enum for each sensor. This is smaller for one
   built-in input, but does not meet external provider admission without core edits.
2. Run separate training and deployment sensor code. This duplicates semantics
   and weakens cross-map validation.
3. Replace the whole scene/runtime system. The current source does not justify
   that cost; it would expand physics and lifecycle risk unnecessarily.

The proposed registry is limited to input production. It does not replace the
existing Robot/Environment registries or introduce a second simulation clock.

## Acceptance and rollout

Follow slices A through F in the detailed design. Require schema tests, compiled
binding/reset tests, unchanged built-in numeric fixtures, analytic ground-query
oracles, two scene bindings, artifact rejection/parity, and an installed external
provider example. Finish with actual packaged UE execution and measured sampling
cost/memory.

Accept this RFC before implementing changed public contracts. Keep application
features outside it. Do not mark the release ready until #51's other gates pass.

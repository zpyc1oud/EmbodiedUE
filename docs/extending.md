# Extending EmbodiedUE

[Project home](../README.md) · [Training guide](training.md)

## Contents

- [Adding a robot](#add-a-robot)
- [Install an external Task](#external-tasks)
- [Extend numeric input declarations](#extend-numeric-input-declarations)


<a id="add-a-robot"></a>

<a id="add-a-robot-adding-a-robot"></a>
## Adding a robot

A robot integration contains UE assets, a Python robot declaration, and a Python Task.
Supported robots share the existing UE runtime, action schema, and reset path.
CartPole and PhantomX are the reference integrations.
See [architecture](architecture.md#architecture-robot-integration) for the supported boundaries.

<a id="add-a-robot-before-you-start"></a>
### Before you start

Complete the [host setup](../README.md#getting-started).
Prepare a robot asset with body and joint names that you can match to its Python declaration.
For a game-deployable Task, choose an exportable observation and action path before training.
See [external Tasks](#external-tasks) if you want to keep your Task in a separate package.

The result of this guide is a registered integration ready for training and physical validation.
Generating files alone does not validate the robot's dynamics.

### Import a frozen Isaac Lab map

Export the Go2 flat surface and one frozen rough tile with Isaac Lab 2.3.2. The fixtures capture the collision triangles, spawn origin, physical material, and PhysX ray probes. Enable the Python Editor Script Plugin in the destination UE 5.8 project.

```powershell
& '<Isaac Lab Python executable>' scripts/export_isaac_go2_terrain.py --output '<fixture-directory>'
uv run python scripts/import_isaac_terrain.py --fixture '<fixture-directory>/slope.json' --project '<project.uproject>' --ue-executable '<UnrealEditor-Cmd.exe>' --level /Game/Maps/IsaacLabSlope --report-dir '<report-directory>'
```

Use `flat.json` for the 24 m × 24 m flat surface. Choose a new UE level package for each import. The importer reopens each saved level to check the bounds, spawn marker, physical material, and collision rays. Use the marked origin when configuring the authored-map spawn.

<a id="add-a-robot-create-starter-files"></a>
### Create starter files

To create starter files, run:

```powershell
uv run uerl new robot phantomx2 --asset /Game/Robots/PhantomX2/SK_PhantomX2
uv run uerl new task walk2 --robot phantomx2 --template phantomx-walk
```

The generator rejects existing output files.
It does not change the explicit registry.
Complete the generated `TODO` topology and Task mathematics before registration.
The value `max_episode_steps: 1000` is a placeholder.

For variable-decimation locomotion, use physical time for rewards, discounts, and episode duration.
See the [PhantomX guide](training.md#phantomx-robust-training).

<a id="add-a-robot-1-ue-assets"></a>
### 1. UE assets

1. Add a Skeletal Mesh, Skeleton, and PhysicsAsset to the host project.
2. Make sure that their topology meets the runtime requirements.
3. Obtain redistribution permission before you contribute meshes, textures, or physics assets.

The runtime reflects structure from these assets.
Python does not duplicate mass, inertia, geometry, or joint limits.
The existing PhantomX mesh is `/Game/Robots/PhantomX/SK_PhantomX`.

<a id="add-a-robot-2-python-robot-declaration"></a>
### 2. Python robot declaration

Add a declaration under `src/uerl/assets/robots/`.
Include the asset path, joint and body names, reference pose, actuator groups, observations, and reset distributions.
Register it in `ROBOT_ASSETS` in `src/uerl/assets/robots/__init__.py`.

Use [cartpole.py](../src/uerl/assets/robots/cartpole.py) and [phantomx.py](../src/uerl/assets/robots/phantomx.py) as examples.

<a id="add-a-robot-3-python-task"></a>
### 3. Python Task

1. Add the Task configuration and builder under `src/uerl/tasks/`.
2. Register the Task in `src/uerl/tasks/registry/defaults.py`.
3. Run `uerl tasks`.
4. Run `uerl check task <TaskID>`.
5. Do a real UE smoke test.

The registered ID is available to `uerl train`, `uerl play`, and `uerl export`.
Use the existing [CartPole](../src/uerl/tasks/cartpole) and [PhantomX](../src/uerl/tasks/phantomx) implementations as references.
Python preflight does not establish correct asset reflection or Chaos behavior.

<a id="add-a-robot-4-train-and-deploy"></a>
### 4. Train and deploy

Train through the same CLI.
Export a `.uerlpol2` artifact.
Compare its observation and action dimensions, topology, and timing with the intended deployment mesh.
Use the [deployment guide](deployment.md#in-game-deployment-guide) and applicable [test layers](../tests/README.md#test-suites).

<a id="external-tasks"></a>

<a id="external-tasks-install-an-external-task"></a>
## Install an external Task

An installed Python package can supply a Task without changes to framework registration.
The [CartPole example](../examples/external-cartpole) changes one reward weight.
It uses the existing CartPole Task factory, Robot declaration, observations, actions, and runtime.

<a id="external-tasks-install-and-enable-the-example"></a>
### Install and enable the example

First install the repository's Python dependencies.
Then run these PowerShell commands:

```powershell
uv pip install ./examples/external-cartpole --no-deps
$env:UERL_TASK_PLUGINS = 'example-cartpole'
uerl tasks
uerl check task Example-CartPole-v0
uerl config --task Example-CartPole-v0 --json
```

After installation, the inspection commands work outside the checkout.
The command `check task` validates Python configuration and Task construction without UE startup.

For your own package:

1. Copy `examples/external-cartpole` outside the repository.
2. Change the package name, entry-point name, and Task ID.
3. Install that directory into the prepared environment.

For editable development, use `uv pip install -e <directory> --no-deps`.
To change `pole_position_weight`, edit `src/example_cartpole/reward.yaml`.
For a non-editable installation, reinstall after a source change.

Alternatively, create a package with the CLI:

```powershell
uv run uerl new external-cartpole balance-demo --output-dir ..\balance-demo
Set-Location ..\balance-demo
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'balance-demo'
uerl tasks --filter UERL-BalanceDemo-v0
uerl check task UERL-BalanceDemo-v0
uerl config --task UERL-BalanceDemo-v0 --json
```

The generated project contains entry-point metadata, a reward resource, registration tests, and usage instructions.
It uses the framework CartPole Task.
Its only Task change is `pole_position_weight` in `src/balance_demo/reward.yaml`.
Run its tests with `python -m pytest`.
After a non-editable source change, reinstall the package.

On a configured Windows/UE host, use the same registration for training:

```powershell
uerl train --task Example-CartPole-v0 --num-envs 2 --max-iterations 1 --device cpu
```

Supply host paths as specified in [Getting started](../README.md#getting-started).
The Python wheel does not contain UE assets, the host project, or UE binaries.

<a id="external-tasks-create-a-directtask-python-hooks-package"></a>
### Create a DirectTask Python-hooks package

The [Direct CartPole example](../examples/external-direct-cartpole) owns its
action, observation, reward, and termination functions in a `DirectTask` subclass.
It still uses the framework's `DirectEnv`, Session, reset, and valid-Slot path.
Its package-owned reward override is YAML, as are Task and Run configurations.

To generate another package from the CLI:

```powershell
uv run uerl new direct-cartpole direct-balance-demo --output-dir ..\direct-balance-demo
Set-Location ..\direct-balance-demo
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'direct-balance-demo'
uerl tasks --filter UERL-DirectBalanceDemo-v0
uerl check task UERL-DirectBalanceDemo-v0 --json
```

`check task --json` writes a machine-readable preflight report to stdout; it is
not a configuration file or a configuration input. The report shows whether
train, evaluate, and export are supported, unsupported, or unknown, with reasons.
For a Python-hooks Task, train/evaluate are supported and export is unsupported
because Python action/observation methods have no Manager-generated plans. Do
not infer mathematical or cross-language exportability from Task metadata.

<a id="external-tasks-registration-contract"></a>
### Registration contract

Declare a named entry point in the package's `pyproject.toml`:

```toml
[project.entry-points."uerl.tasks"]
my-task = "my_package:create_registration"
```

The target must be a zero-argument callable that returns one `uerl.tasks.registry.TaskRegistration`.
Its factories supply fresh Task, Worker, and runner configurations and a `DirectTask`.
Optional curriculum, event, and evaluation factories remain available.

Each Task owns its Python resources.
Use `importlib.resources` to load them.
Do not locate them through the current directory.

Enable entry-point names with the comma-separated `UERL_TASK_PLUGINS` environment variable.
Installation alone does not enable a package.
All CLI commands use this selection.
Python callers can use `create_default_registry(external_tasks=("my-task",))`.
An explicit `()` selects built-in Tasks only and overrides the environment variable.
Only enable trusted packages, because package loading executes their Python code.

Task IDs must be unique across built-in Tasks and enabled packages, including different versions.
Entry-point names must be unique among installed packages.
Missing names, import failures, invalid callables, invalid registration objects, and invalid factories cause `RegistryError`.
The error identifies the entry-point source.
A failed construction does not return a partial registry.
Keep the same package and selection available for saved Runs.

Registration does not require Manager composition.
An external factory can return a `DirectTask` subclass or a composed Task.
Both use DirectEnv, Session, reset, and valid-Slot behavior.
This example uses the existing composed CartPole factory without a new simulation loop.

<a id="external-tasks-simple-and-manager-task-implementations"></a>
### Simple and Manager Task implementations

An external factory can return either a `DirectTask` subclass that implements
`preprocess_actions`, `build_observations`, `compute_terminations` (or
`termination_terms`), and `compute_rewards`, or a `DirectTask` assembled from
action, observation, termination, and reward Managers. `DirectTask`,
`DirectTaskCfg`, `UERLDirectEnv`, and the capability report types are public
Python APIs. Both Task styles use the same `DirectEnv` step/reset path; Task code
does not own Session calls, sparse reset, or invalid-Slot compaction.

Inspect a constructed Task with `task.capabilities`. Training and evaluation are
supported when the required DirectTask methods or Manager declarations exist.
Manager Tasks report `unknown` until a Worker RobotSpec has been bound and their
plans have been assembled. A Python-only Task can train and evaluate, but its
Python observation/action math has no exported plan and reports export as
`unsupported` with the reason. The export service stops such a Task before
calling the ONNX exporter.

Training and evaluation report capabilities after Worker schema binding and
before their long loop. A known unsupported capability fails before Worker
startup; Manager `unknown` is allowed to reach RobotSpec binding and then must
resolve before the loop starts. Export rejects known unsupported work before
Worker startup, then checks strictly after DirectEnv initialization and its
initial reset, before vector-runner construction or checkpoint loading. It
proceeds only when the resolved report says `supported`; both `unsupported` and
`unknown` stop first. If a Task adds Python action or
observation behavior around Manager plans, export reports `unknown`: the
framework does not infer mathematical equivalence from registration metadata.
Resolve the implementation into a supported plan path before export, then
complete the separate artifact, UE, and target-scene checks for the behavior you
intend to ship.

<a id="external-tasks-built-in-resources"></a>
### Built-in resources

Built-in training and terrain YAML files are under `src/uerl/configs/`.
Wheels and source distributions include these files.
The existing loaders read this installed location.
Logical resource IDs, such as `environments/terrains/cartpole/flat.yaml`, remain unchanged.

Use Task loaders instead of the old checkout path `configs/tasks/...`.
The function `repository_config_root()` now returns the package resource root for standard wheel and editable installations.
Path-based loaders do not support direct import from a zipped wheel.

<a id="external-tasks-validation-boundary"></a>
### Validation boundary

The packaging test builds and installs the framework, checked-in examples, and
generated Manager and Direct package wheels into an isolated target. It checks
entry-point discovery, YAML resource loading, Task construction, and CLI
capability reports outside the checkout, then executes one scripted DirectEnv
step for each installed Direct package outside the checkout. The Direct/Manager
behavior test uses the checked-in Direct Task implementation, a Manager
registration, a scripted Session, and independent numeric expectations for
actions, observations, rewards, termination, reset masks, and invalid Slots.
These are Python checks; they do not validate UE execution, Chaos behavior,
training quality, or game deployment. Those remain separate Issue #6 acceptance
gates.

See [Write tests](../tests/README.md#write-tests-write-package-and-generator-tests) for the test procedure.
[Issue #6](https://github.com/zpyc1oud/EmbodiedUE/issues/6) tracks the remaining external Task acceptance.

## Extend numeric input declarations

Worker and game hosts resolve the same versioned numeric input declarations.
Put ordered `InputSpec` values in `WorkerConfig.inputs`. Describe publishes the
effective parameters and output fields. A custom Task selects those descriptors
in its `SessionSchema` and consumes them through ordinary observation-plan
`select` operations. Existing Robot observations keep their current meaning.

For example, this declaration routes two existing CartPole fields through the
state input adapter:

```python
from uerl.core.inputs import InputSpec

inputs = (InputSpec(
    name="proprioception", provider_id="uerl.state", provider_version=1,
    parameters={"fields": [
        "robot.joint.pole.joint_position",
        "robot.joint.cart.joint_velocity",
    ]},
),)
```

Assign `inputs` when constructing the Task registration's `WorkerConfig`.
The CLI resolver also accepts a JSON sequence through `--worker.inputs`.
Saved Run configuration retains these declarations; the Manifest's effective
Worker configuration retains the resolved specifications and descriptors.
Export reads the initialized Session contract and writes it into the policy.
Snapshots created before `inputs` existed restore with an empty input list.

### Declare an input

Use `InputSpec` from `uerl.core.inputs`. Supply an instance name, provider ID and
positive provider version. Parameters are immutable YAML-compatible values.
Attachment and scene binding names describe logical roles, not map actor paths.
The first supported modes are `completed_window` sampling and `fault` on missing
required data.

Implement a Python `InputFactory` with `provider_id`, `version`, and `validate`.
Validate provider-specific parameters, add explicit defaults, then return a
`ResolvedInput` with `InputField` descriptors. Register the factory on an explicit
`InputRegistry`. Compile the ordered declarations once before allocating buffers.
The result contains effective specifications, field descriptors and stable offsets.

Output fields use float32 values. Declare shape, unit, frame, semantic and source.
An empty shape `[]` is a scalar; `[1]` is a one-element vector.
Duplicate field names fail compilation, including case-only differences that
collide in native FName lookup. Unsupported provider versions fail before binding.
Input declaration tests use an external factory without a core dispatch branch.

### Implement the native provider

Implement `IUERLInputFactory` and `IUERLInputProvider` from `UERLInputProvider.h`.
Register catalog-independent factories during plugin startup. Register factories
that depend on a host's state catalog in a host-local registry. The factory validates
owned parameter object text and effective descriptors, then creates the native instance.
Parameters are encoded internally by the host; human configuration remains YAML.

`FUERLCompiledInputSet` validates every declaration and logical binding before
creating instances. Bind copies required weak object references. A provider owns
its buffers and callbacks, not the Robot, World, or simulator clock.

The host supplies a completed sequence, solver time and reset generation.
Repeated reads return the cached publication. A provider must write every declared
scalar. Missing or non-finite values fail the Slot rather than publish zeros.
The Slot requires reset after a sampling fault.

Reset accepts stable Slot IDs. It increments only selected generations and leaves
other caches unchanged. Read `GetResetGeneration` when preparing the next host
sample stamp. Reset does not advance physics or produce a new observation.
Release providers before unloading their module or destroying their World.

### Adapt an existing state snapshot

`StateInputFactory` declares `uerl.state` version 1. Supply the host's existing
`InputField` catalog to its constructor and register it on an `InputRegistry`.
The native counterpart is `FUERLStateInputFactory` in `UERLStateInput.h`, constructed
with the matching `FUERLFieldDescriptor` catalog on a host-local registry.

Select fields with `parameters={"fields": ["body_velocity", "support"]}` using
names present in that catalog. The explicit list determines output order.
Resolution retains the catalog's names, shapes, units, frames, semantics and
sources. Names resolve case-insensitively and effective parameters retain the
catalog's spelling. Two inputs cannot publish the same field.

For each native Slot, populate `FUERLInputSlotBinding::StateReaders` with a
`FUERLInputStateReader` for each selected field. Its descriptor must match the
catalog. Its callback returns an already collected float32 view and the snapshot's
actual completed sequence, solver time and reset generation. These must equal the
requested sample stamp. The adapter copies the values into the compiled input
set's publication buffer, so repeated reads retain the completed publication.

The host owns snapshot collection and physical resets. After a sparse reset,
publish the selected Slots' reset snapshots with `GetResetGeneration` before
sampling them. The adapter's reset leaves host data and unselected Slots alone.
Release the compiled input set before destroying callback owners; callbacks using
weak owners must return a diagnostic if their source is gone.

#### Limitations

The state adapter preserves the existing host frame. Missing data, wrong widths,
stale snapshots and non-finite values fault the sampled Slot and require reset
before recovery. The current hosts expose `ground` as their scene binding and
`root` plus reflected body names as attachment transforms. Additional scene roles
require an explicit host binding implementation.

### Validate the extension

Follow [Write tests](../tests/README.md#write-tests). The current native foundation cases are
listed in [UE tests](../tests/README.md#numeric-input-provider-foundation).
A deterministic provider proves field offsets, cache behavior and lifecycle only.
Real terrain sampling, scene filtering, serialization parity and installed-plugin
inference require their own behavior tests.

### Configure ground rays

`RayGroundFactory` declares `uerl.ray_ground` version 1. The native factory uses
`MakeUERLRayGroundInputFactory` and registers with the Robot module. It does not
replace the legacy Robot observations in this slice.

The effective parameters are:

- `offsets_m`: ordered XY sample positions; default `[[0, 0]]`
- `start_height_m`: positive height above the attachment; default 1 metre
- `end_depth_m`: positive depth below the attachment; default 2 metres
- `alignment`: `yaw` rotates offsets by attachment yaw; `world` leaves them fixed
- `output`: `height` returns hit Z minus attachment Z; `clearance` reverses the sign

Distances must remain finite after conversion to UE centimetres. The rays point
along world -Z. Attachment scale does not scale the configured metre offsets.
The output frame is `world/z-relative`, with one float32 metre value per offset.

An attachment is required. The host supplies a read-only transform callback for
its logical name. The callback reads the completed physical state in world
centimetres. Bind copies this callable; its host-owned objects must remain valid
until release. Native output rejects a non-finite transform or non-unit rotation.

One logical ground binding is required; the default name is `ground`. Both hosts
supply a nonempty actor whitelist. The query uses that list and the Slot collision
scope in training and deployment. The game does not bypass the list with a global
WorldStatic fallback. There is no hold-last-hit behavior; a missing required hit
fails the sample. Changing the old fallback, ray origin or output meaning requires
reevaluation and can require fresh training when this path is adopted.

The native `UERL.Unit.Robot.GroundQuery.DeclaredProvider` case constructs two
translated scene bindings with a plane, a step and an excluded overhead surface.
Its independent expected heights are -1 and -0.5 metres; clearance is +1 metre.
Compilation and execution on UE remain required before runtime acceptance.

### Install an independent declaration package

[The external input example](../examples/external-input/README.md) supplies a
Python wheel and a matching native plugin source. Its constant scalar makes the
expected result independent of physics. The tooling test builds both Python
packages, installs them outside the checkout, removes the staged source and
resolves the packaged YAML through the public registry.

This proves declaration packaging. The native example must still be built and
sampled in a separate UE project before claiming installed-plugin runtime support.

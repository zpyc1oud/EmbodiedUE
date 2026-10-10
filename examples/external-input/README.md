# External numeric input example

This example defines `example.constant` version 1 in an independent Python package
and a native UE plugin. It demonstrates registration and field publication, not a
physical sensor or a trained policy.

Install the Python package from this directory after installing the matching
EmbodiedUE development package:

```powershell
python -m pip install .
```

Use `example_input.register_inputs(registry)` on an `InputRegistry`, then compile
`example_input.example_spec()`. The packaged YAML declares value 2.5 and the output
field is `input.example.value`, shape `[1]`, unit `1`, frame `none`.

The native source is in `ue/ExampleInput`. In a separate UE validation project,
copy that directory under `Plugins` alongside the matching UERLEngine plugin,
then build the Editor target. Plugin startup registers the native factory.
Release compiled input sets before unloading the plugin. Human configuration is
YAML; the `.uplugin` file is the engine's required manifest format.

The current core slice does not yet load these declarations through a Task or
policy artifact. An installed Python test establishes packaging and declaration
compilation only. Native plugin build, actual output, Worker/game selection and
packaged inference remain validation gates. Do not treat this example as a
working end-to-end deployment command.

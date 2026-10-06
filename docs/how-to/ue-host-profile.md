# Reuse a host profile

Save the UE executable and host project paths in one profile.
The commands `train`, `play`, and `export` use this profile when they launch a Worker.
Run `uerl check host` from the installed Python environment for static file checks.

This preflight applies to the bundled Windows x64 / UE 5.8 CartPole host.
First complete the [installation and build instructions](../../README.md#install-and-build).

## Create the local profile

Create `%USERPROFILE%\.uerl\host.toml` outside the repository.
The profile is host-specific configuration.
Both fields are optional:

```toml
ue_executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
project = 'D:\work\EmbodiedUE\engine\UERLHost.uproject'
```

Replace the paths with your host paths.
TOML single-quoted strings preserve Windows backslashes.
For `project`, select a `.uproject` file, not a directory.

Run the static checks:

```powershell
uv run uerl check host
uv run uerl check host --json
```

To select another profile, use `--host-profile` or the environment variable:

```powershell
uv run uerl check host --host-profile 'D:\profiles\lab.toml'
$env:UERL_HOST_PROFILE = 'D:\profiles\lab.toml'
uv run uerl check host
uv run uerl check host --project 'D:\other\engine\UERLHost.uproject'
```

The command reads files only.
It does not create the profile or change project, plugin, driver, environment, or system settings.

## Resolution rules

| Selection | Priority, highest first |
|---|---|
| Profile file | `--host-profile`, `UERL_HOST_PROFILE`, `~/.uerl/host.toml` |
| Executable | `--ue-executable`, profile `ue_executable`, Epic UE 5.8 default |
| Project | `--project`, profile `project`, checkout `engine/UERLHost.uproject` |

The Epic default is `C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe`.
The checkout default is relative to the installed Python source, as in the existing runtime commands.
For another installation, set `project` explicitly.

If the implicit default profile is absent, the command uses default values.
An explicitly selected profile must exist.
This also applies to a profile selected through the environment variable.

Invalid TOML, unknown fields, empty values, and non-string values cause failure.
A command-line override does not hide an invalid profile field.
Profiles do not merge.

A relative profile value is relative to the profile directory.
A relative command-line path is relative to the current directory.
The resolver expands `~`.
It does not expand environment variables inside field values or read `UE_ROOT`.
The host that executes the command interprets the paths.

## Read and reuse the result

| Exit status | Meaning |
|---|---|
| `0` | All static checks passed |
| `1` | Configuration or static check failure |
| `2` | Invalid CLI use |

Text output gives the selected paths, their sources, check codes, and corrective actions.
Source values are `explicit`, `profile`, or `default`.
JSON output contains `ok`, `scope`, `platform`, `selected`, and `checks`.
A profile read or parse failure returns `ok: false` and an `error` with `code` and `message`.

In launch mode, `train`, `play`, and `export` use these same resolution rules.
After static preflight passes, run:

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --device cpu
$runDir = 'runs/UERL-CartPole-Direct-v0/<run-directory>'
uv run uerl play --task UERL-CartPole-Direct-v0 --run $runDir
uv run uerl export --task UERL-CartPole-Direct-v0 --run $runDir
```

All three commands accept `--host-profile`, `--ue-executable`, and `--project`.
An explicit path replaces only its own field.
An executable override therefore retains the profile's project path.

Advanced overrides retain priority over computed launch values, including explicit path flags.
These overrides are `--session.worker_executable` and `--session.worker_args`.
The latter replaces the complete argument list.
Map and port precedence remain unchanged.

With `--session.mode attach`, the commands ignore host profiles and local launch flags.
External-Worker ownership rules remain unchanged.
An absent or malformed local profile cannot prevent attachment.

Host paths supply Session launch settings only.
They do not replace saved Worker, Task, or runner settings.
They do not edit the source Run or change continuation rules.
The resolved-configuration hash includes Session settings, so host path changes can change that hash.
The hash is not a semantic compatibility test.

The profile does not select a deployment target or change `deploy --check` or the E2E runner.
Runtime commands launch UE, but `check host` does not.
Runtime commands resolve the profile without automatically doing the static file checks.

Python callers can use `uerl.host.resolve_host_profile(profile_path=...)` and `uerl.host.check_host(profile)`.
Resolution returns selected `Path` values, `profile_path`, and field `sources`.
It does not require the selected files to exist.
The report contains `checks` and an `ok` property.
These APIs do not import the training runner or saved-run resolver.

## Checked files and repairs

| Check | Corrective action |
|---|---|
| Executable and Windows engine directory layout | Select UE 5.8 `Engine/Binaries/Win64/UnrealEditor-Cmd.exe`. |
| `.uproject` JSON and explicit plugin enablement | Use the bundled host structure. Enable UERLEngine, ProceduralMeshComponent, and NNERuntimeORT. |
| Plugin descriptor JSON | Restore the bundled UERLEngine plugin or install the missing engine dependency. |
| Host and Worker DLL presence | Build `UERLHostEditor Win64 Development` with the selected engine. |
| Engine Entry map and four CartPole files | Repair engine content or run `git lfs pull` in the host checkout. |

Missing, unreadable, empty files and Git LFS pointers cause failure.
The default CartPole map is `/Engine/Maps/Entry`.
The robot files are `SK_CartPole`, `SKM_CartPole`, `PA_CartPole`, and `CartPole` under `Content/Robots/CartPole`.
This preflight does not include custom maps or PhantomX resources.

## Limits

Static success does not establish runtime readiness.
It does not validate UE version, DLL compatibility or age, transitive asset references, asset loading, GPU availability, physics, or training.
After preflight, do the documented Windows build and smoke test.

Linux cases with dummy files establish resolution and diagnostic behavior only.
Startup probes, process cleanup, and independent Windows first-use validation remain in [Issue #5](https://github.com/zpyc1oud/EmbodiedUE/issues/5).

# Reuse a host profile for static checks

Store your UE executable and host project paths once, then run `uerl check host`
from the installed Python environment. This preflight targets the bundled
Windows x64 / UE 5.8 CartPole host. Follow the [installation and build
instructions](../../README.md#install-and-build) first.

## Create the local profile

Create `%USERPROFILE%\.uerl\host.toml`. The file is local machine configuration;
keep it outside the repository. Its two optional fields are:

```toml
ue_executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
project = 'D:\work\EmbodiedUE\engine\UERLHost.uproject'
```

Replace both paths with your own. TOML single-quoted strings preserve Windows
backslashes. Use a `.uproject` file, not its directory.

```powershell
uv run uerl check host
uv run uerl check host --json
```

Select a different profile explicitly, or set a variable for the current shell:

```powershell
uv run uerl check host --host-profile 'D:\profiles\lab.toml'
$env:UERL_HOST_PROFILE = 'D:\profiles\lab.toml'
uv run uerl check host
uv run uerl check host --project 'D:\other\engine\UERLHost.uproject'
```

The command reads files only. It neither creates the profile nor changes project,
plugin, driver, environment, or system settings.

## Resolution rules

| Selection | Priority, highest first |
|---|---|
| Profile file | `--host-profile`, `UERL_HOST_PROFILE`, `~/.uerl/host.toml` |
| Executable | `--ue-executable`, profile `ue_executable`, Epic UE 5.8 default |
| Project | `--project`, profile `project`, checkout `engine/UERLHost.uproject` |

The Epic default is
`C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe`.
The checkout default is relative to the installed Python source, as with the
existing runtime commands. Set `project` explicitly for other installations.

A missing implicit default profile uses defaults. An explicitly selected or
environment-selected file must exist. Invalid TOML, unknown fields, and empty or
non-string values fail, even when a command overrides those fields. Profiles are
not merged. A relative path inside a profile is relative to that profile's
folder; a relative command-line path is relative to the current working folder.
`~` is expanded. Environment variables inside field values are not expanded;
`UE_ROOT` is not read. Paths are interpreted by the machine running the check.

## Read and reuse the result

Exit status is `0` when all static checks pass, `1` for configuration or check
failures, and `2` for invalid CLI usage. Text output includes the selected paths,
their sources (`explicit`, `profile`, or `default`), check codes and corrective
actions. JSON exposes `ok`, `scope`, `platform`, `selected`, and `checks`. A profile
parse/read failure returns `ok: false` and an `error` with `code` and `message`.

`train`, `play`, and `export` still use their existing flags; they do not yet load
this profile automatically. Until those entry points are integrated, pass the
selected paths explicitly. For example, after the preflight passes:

```powershell
$report = uv run uerl check host --json | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Resolve the reported host checks first.' }
$hostArgs = @('--ue-executable', $report.selected.ue_executable, '--project', $report.selected.project)
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --device cpu @hostArgs
```

Reuse `@hostArgs` with `play` and `export`. Training, playback and export start UE;
the check itself does not. This profile does not select a deployment target or
change `deploy --check` or the E2E test runner.

Python callers can use `uerl.host.resolve_host_profile(profile_path=...)` and
`uerl.host.check_host(profile)`. Resolution returns selected `Path` values,
`profile_path` and field `sources`; it does not require the selected files to exist.
The report contains `checks` and an `ok` property. These APIs do not import the
training runner or saved-run resolver.

## Checked files and repairs

| Check | Corrective action |
|---|---|
| Executable and Windows engine directory layout | Select the UE 5.8 `Engine/Binaries/Win64/UnrealEditor-Cmd.exe` |
| `.uproject` JSON and explicit plugin enablement | Use the bundled host structure; enable UERLEngine, ProceduralMeshComponent and NNERuntimeORT |
| UERLEngine and required engine plugin descriptor JSON | Restore the bundled plugin or install the missing engine dependency |
| Host and Worker DLL presence | Build `UERLHostEditor Win64 Development` using the selected engine |
| Engine Entry map and the four bundled CartPole asset files | Repair engine content or run `git lfs pull` in the host checkout |

Missing, unreadable, empty files and Git LFS pointers fail. The default CartPole
map is `/Engine/Maps/Entry`; the robot files are `SK_CartPole`, `SKM_CartPole`,
`PA_CartPole`, and `CartPole` under `Content/Robots/CartPole`. Custom maps and
PhantomX resources are outside this first preflight.

## Limits

Static success does not validate UE version, DLL ABI/freshness, transitive asset
references, asset loading, GPU/device availability, physics or training. File
presence cannot establish runtime readiness. Run the documented Windows build
and smoke test afterwards. Linux tests with dummy files validate only resolution
and diagnostics; they do not validate Windows or real UE. Startup probing,
process cleanup and independent Windows first-use validation remain part of
[Issue #5](https://github.com/zpyc1oud/EmbodiedUE/issues/5).

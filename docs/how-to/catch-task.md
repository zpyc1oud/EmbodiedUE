# Contact-based Catch prototype

Task: `UERL-PhantomX-Catch-v0`.
Design and pending acceptance: [RFC 0003](../rfcs/0003-contact-catch-task.md).

The current prototype creates a single-Robot arena in `/Engine/Maps/Entry`.
It uses Engine primitive meshes to show a human-shaped target. No external demo
asset is required. The Environment moves the target using completed physics time.

Capture requires a blocking hit from the bound Robot during a Worker-owned control
window. Distance alone does not capture. A fall takes precedence over capture.
Reset clears the latch and returns the target to the start of its path.

## Run the prototype

Use a built host with the Catch candidate's plugin:

```powershell
uv run uerl config --task UERL-PhantomX-Catch-v0
uv run uerl train --task UERL-PhantomX-Catch-v0 --num-envs 1 --max-iterations 1 --run-dir runs/catch-smoke
uv run uerl play --run runs/catch-smoke --steps 100 --presentation none
```

These commands check task wiring. One training iteration does not establish learned
capture. Preserve the Run and record the actual candidate and host revisions.

Evaluation reports capture, fall, timeout and Slot-fault rates separately.
Linear and yaw RMSE use valid physical time. Capture time resets only for completed
Slots. An evaluation with no valid physical samples reports unavailable errors.

## Test the interaction

```powershell
uv run python scripts/run_e2e.py --suite ue -k catch
```

This case drives the physical target into the Robot, checks the actual contact
latch, and then checks reset. It does not measure a learned pursuit policy.
Run the native Catch group after building the candidate. The complete test runner
includes its unit ownership checks and real Chaos contact case.

## Remaining acceptance

Native build and interaction evidence, trained behavior, a saved game scene and
in-game policy deployment must be verified before this prototype is accepted.
The optional player-target configuration requires a possessed Pawn with a collidable
primitive root. Keep source-owned proxy content separate from externally licensed
assets when preparing the final package.

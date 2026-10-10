"""Run the repository's complete validation: Python, UE unit, and UE E2E suites."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

PHYSICS_RESPONSE_CASES: tuple[str, ...] = (
    'UERL.Integration.PhysicsResponse.Contact.RestitutionAndCollisionImpulse',
    'UERL.Integration.PhysicsResponse.Contact.StaticAndSlidingFriction',
    'UERL.Integration.PhysicsResponse.Contact.StaticWeightAndImpulseBalance',
    'UERL.Integration.PhysicsResponse.Contact.ZeroFrictionPreservesMomentum',
    'UERL.Integration.PhysicsResponse.FreeBody.AsymmetricPrincipalInertia',
    'UERL.Integration.PhysicsResponse.FreeBody.COMAndPointVelocityIdentity',
    'UERL.Integration.PhysicsResponse.FreeBody.ForceMassTrajectoryAndOracleSensitivity',
    'UERL.Integration.PhysicsResponse.FreeBody.GravityAndSelectedCompensation',
    'UERL.Integration.PhysicsResponse.FreeBody.ImpulseAndForceLifetime',
    'UERL.Integration.PhysicsResponse.FreeBody.OffsetForceWorldAndLocalFrames',
    'UERL.Integration.PhysicsResponse.FreeBody.TorqueInertiaTrajectoryAndOracleSensitivity',
    'UERL.Integration.PhysicsResponse.Joint.AngularDampingDecay',
    'UERL.Integration.PhysicsResponse.Joint.CoordinateSignAndLockedAxes',
    'UERL.Integration.PhysicsResponse.Joint.HardLimitsAndDisabledDrive',
    'UERL.Integration.PhysicsResponse.Joint.LoadedPositionDriveEquilibrium',
    'UERL.Integration.PhysicsResponse.Joint.PositionDriveTorqueSaturation',
    'UERL.Integration.PhysicsResponse.Joint.UnderdampedAndOverdampedStepResponse',
    'UERL.Integration.PhysicsResponse.PhantomX.AssetAndLiveParameterInventory',
    'UERL.Integration.PhysicsResponse.PhantomX.FixedTargetPositionVelocityConsistency',
    'UERL.Integration.PhysicsResponse.PhantomX.TotalMomentumUnderExternalImpulse',
    'UERL.Integration.PhysicsResponse.PhantomX.NamedRoutingAndIndependentState',
    'UERL.Integration.PhysicsResponse.PhantomX.WholeBodyWeightSupport',
)

# PIEAttach is run by the E2E suite, which supplies its required loopback port
# and Python client. Keep the remaining Automation filters complete for the
# currently registered Unit and Integration families. Run the added terrain
# group in a fresh process so preview-scene physics is independent of prior
# Automation state.
UE_AUTOMATION_GROUPS: tuple[tuple[str, str, int], ...] = (
    (
        "unit and core integration",
        "UERL.Unit+"
        "UERL.Integration.Worker.SlotCollision+"
        "UERL.Integration.Worker.SharedWorldCollision+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003+"
        "UERL.Integration.Policy.Contact+"
        "UERL.Integration.Policy.Ground+"
        "UERL.Integration.Policy.Clock+"
        "UERL.Integration.Policy.Controller+"
        "UERL.Integration.Policy.Component",
        142,
    ),
    (
        "additional robot, environment-pool, and terrain integration",
        "UERL.Integration.Robot.GenericDrive+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_001+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_002+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_004+"
        "UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_005+"
        "UERL.Integration.Robot.TopologyReflector+"
        "UERL.Integration.Worker.EnvironmentPool+"
        "UERL.Integration.Worker.Terrain",
        13,
    ),
    (
        "quantitative physics response",
        "UERL.Integration.PhysicsResponse",
        len(PHYSICS_RESPONSE_CASES),
    ),
)


sys.path.insert(0, str(REPO_ROOT))
sys.path.insert(0, str(REPO_ROOT / "src"))



def main(argv: list[str] | None = None) -> int:
    from scripts.run_e2e import pytest_command
    from scripts.test_runner_support import TestRun, add_runner_flags

    parser = argparse.ArgumentParser(description=__doc__)
    add_runner_flags(parser)
    args = parser.parse_args(argv)
    names = ["Python", *(name for name, _, _ in UE_AUTOMATION_GROUPS), "UE E2E"]
    run = TestRun(args.output_dir, names, args.timeout)
    status = run.run(0, [sys.executable, "-m", "pytest", "-q"])
    if status:
        return status
    if not run.configure_host(args, 1):
        return 2
    assert run.host is not None
    for index, (_, filters, minimum) in enumerate(UE_AUTOMATION_GROUPS, start=1):
        log = run.directory / f"{index + 1:02d}.ue.log"
        command = [
            str(run.host.ue_executable), str(run.host.project), "/Engine/Maps/Entry",
            f"-ExecCmds=Automation RunTests {filters};Quit", f"-abslog={log}",
            "-unattended", "-nullrhi", "-nosound", "-NoSplash",
        ]
        status = run.run(index, command)
        if status:
            return status
        status = run.check_automation(
            index, log, minimum,
            required_names=PHYSICS_RESPONSE_CASES if filters == "UERL.Integration.PhysicsResponse" else (),
        )
        if status:
            return status
    return run.run(len(names) - 1, pytest_command("all"))


if __name__ == "__main__":
    raise SystemExit(main())

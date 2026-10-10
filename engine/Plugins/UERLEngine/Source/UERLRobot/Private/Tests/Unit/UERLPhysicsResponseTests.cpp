#include "Misc/AutomationTest.h"

#include "Chaos/ChaosEngineInterface.h"
#include "Chaos/RigidParticles.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PreviewScene.h"
#include "UERLGenericRobotCommandApplier.h"
#include "UERLGenericRobotKinematics.h"
#include "UERLPhysicsSnapshot.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UERLPhysicsResponseTests
{
	/** Test-local settings, restored before another Automation case can run. */
	struct FLockstepSettings
	{
		FLockstepSettings()
		{
			UPhysicsSettings* S = GetMutableDefault<UPhysicsSettings>();
			Async = S->bTickPhysicsAsync;
			Substeps = S->bSubstepping;
			AsyncSubsteps = S->bSubsteppingAsync;
			S->bTickPhysicsAsync = false;
			S->bSubstepping = false;
			S->bSubsteppingAsync = false;
		}
		~FLockstepSettings()
		{
			UPhysicsSettings* S = GetMutableDefault<UPhysicsSettings>();
			S->bTickPhysicsAsync = Async;
			S->bSubstepping = Substeps;
			S->bSubsteppingAsync = AsyncSubsteps;
		}
		bool Async = false;
		bool Substeps = false;
		bool AsyncSubsteps = false;
	};

	TUniquePtr<FPreviewScene> MakeScene()
	{
		return MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false).SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true).SetTransactional(false).SetEditor(false));
	}

	UStaticMeshComponent* MakeCube(UWorld& World, double MassKg)
	{
		AActor* Owner = World.SpawnActor<AActor>();
		if (!Owner) { return nullptr; }
		UStaticMeshComponent* Cube = NewObject<UStaticMeshComponent>(Owner);
		Cube->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Cube->SetMobility(EComponentMobility::Movable);
		// The engine cube is 100 cm wide. This fixture is a uniform 20 cm cube.
		Cube->SetWorldScale3D(FVector(0.2));
		Cube->SetWorldLocationAndRotation(FVector(0.0, 0.0, 500.0), FRotator(17.0, 31.0, -13.0));
		Cube->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Cube->SetCollisionResponseToAllChannels(ECR_Ignore);
		Owner->AddInstanceComponent(Cube);
		Cube->RegisterComponent();
		Cube->SetEnableGravity(false);
		Cube->SetLinearDamping(0.0f);
		Cube->SetAngularDamping(0.0f);
		Cube->SetSimulatePhysics(true);
		Cube->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);
		// An analytic free body must not lose velocity to the engine's sleep
		// heuristic. Match the production Robot's never-sleep physical policy.
		if (FBodyInstance* Body = Cube->GetBodyInstance())
		{
			if (FPhysicsActorHandle Handle = Body->GetPhysicsActorHandle())
			{
				Handle->GetGameThreadAPI().SetSleepType(Chaos::ESleepType::NeverSleep);
			}
		}
		Cube->WakeAllRigidBodies();
		return Cube;
	}

	bool Tick(FAutomationTestBase& Test, UWorld& World, double Dt)
	{
		FString Error;
		FUERLSolverClockSnapshot Before, After;
		if (!ReadUERLSolverClock(World, Before, Error)) { Test.AddError(Error); return false; }
		World.Tick(ELevelTick::LEVELTICK_All, static_cast<float>(Dt));
		++GFrameCounter;
		if (!ReadUERLSolverClock(World, After, Error)) { Test.AddError(Error); return false; }
		const bool Valid = After.Frame - Before.Frame == 1
			&& FMath::Abs(After.LastDt - Dt) < 2.0e-7
			&& FMath::Abs(After.SolverTime - Before.SolverTime - Dt) < 2.0e-6;
		if (!Valid) { Test.AddError(TEXT("analytic fixture did not advance exactly one declared solver step")); }
		return Valid;
	}

	bool Matches(const FVector& Actual, const FVector& Expected, double AbsoluteTolerance)
	{
		return !Actual.ContainsNaN() && (Actual - Expected).Size() <= AbsoluteTolerance;
	}

	UPhysicsConstraintComponent* PinCube(UStaticMeshComponent& Cube, double Kp, double Kd, double Limit)
	{
		AActor* Owner = Cube.GetOwner();
		UPhysicsConstraintComponent* Joint = NewObject<UPhysicsConstraintComponent>(Owner);
		Joint->SetWorldLocation(Cube.GetBodyInstance()->GetCOMPosition());
		Joint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
		Joint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
		Joint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
		Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Free, 0.0f);
		Joint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Locked, 0.0f);
		Joint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Locked, 0.0f);
		Owner->AddInstanceComponent(Joint);
		Joint->RegisterComponent();
		// Frame1 is the moving cube; Frame2 is the fixed World anchor at its COM.
		Joint->SetConstrainedComponents(&Cube, NAME_None, nullptr, NAME_None);
		Joint->ConstraintInstance.DisableProjection();
		FUERLActuatorConfig Actuator;
		Actuator.CoordinateType = TEXT("revolute");
		Actuator.Coordinate = UERLJointCoordinateName(EUERLJointCoordinate::Twist);
		Actuator.TargetMode = TEXT("position");
		Actuator.Stiffness = Kp;
		Actuator.Damping = Kd;
		Actuator.EffortLimit = Limit;
		ConfigureGenericRobotRevoluteAngularDrive(Joint->ConstraintInstance, Actuator);
		Joint->ConstraintInstance.SetAngularVelocityTarget(FVector::ZeroVector);
		return Joint;
	}

	double JointPosition(UStaticMeshComponent& Cube, FConstraintInstance& Joint)
	{
		return MeasureGenericJointPosition(
			Joint.GetRefFrame(EConstraintFrame::Frame1) * Cube.GetBodyInstance()->GetUnrealWorldTransform(),
			Joint.GetRefFrame(EConstraintFrame::Frame2), EUERLJointCoordinate::Twist);
	}


	void ReportHinge(FAutomationTestBase& Test, UStaticMeshComponent& Cube,
		FConstraintInstance& Joint, const TCHAR* Trial, int32 Step)
	{
		FBodyInstance* Body = Cube.GetBodyInstance();
		const FVector W = Body->GetUnrealWorldAngularVelocityInRadians();
		const FVector I = Body->GetBodyInertiaTensor() / 10000.0;
		FVector ReactionForce, ReactionTorque;
		Joint.GetConstraintForce(ReactionForce, ReactionTorque);
		const FVector ProfileTarget = Joint.GetAngularVelocityTarget();
		FVector SolverTarget = FVector::ZeroVector;
		FPhysicsInterface::GetDriveAngularVelocity(Joint.GetPhysicsConstraintRef(), SolverTarget);
		const auto& Drive = Joint.ProfileInstance.AngularDrive;
		Test.AddInfo(FString::Printf(
			TEXT("[PHYSICS_DIAGNOSTIC] trial=%s step=%d mass=%.9f I_si=(%.9f,%.9f,%.9f) q=%.9f w=(%.9f,%.9f,%.9f) reaction_nm=(%.9f,%.9f,%.9f) kp_si=%.9f kd_si=%.9f limit_nm=%.9f acceleration=%d projection=%d profile_target_rev_s=(%.9f,%.9f,%.9f) solver_target_rad_s=(%.9f,%.9f,%.9f)"),
			Trial, Step, Body->GetBodyMass(), I.X, I.Y, I.Z, JointPosition(Cube, Joint),
			W.X, W.Y, W.Z, ReactionTorque.X / 10000.0, ReactionTorque.Y / 10000.0, ReactionTorque.Z / 10000.0,
			Drive.TwistDrive.Stiffness / 10000.0, Drive.TwistDrive.Damping / 10000.0,
			Drive.TwistDrive.MaxForce / 10000.0, Drive.bAccelerationMode, Joint.IsProjectionEnabled(),
			ProfileTarget.X, ProfileTarget.Y, ProfileTarget.Z, SolverTarget.X, SolverTarget.Y, SolverTarget.Z));
	}

	bool RunFreeBody(FAutomationTestBase& Test, bool Angular)
	{
		FLockstepSettings Settings;
		// These are deliberate *applied-input* corruptions, while the declared SI
		// command and independent expected response remain unchanged. The oracle
		// must reject absent input, reversed input, and a 100x conversion error.
		for (double InputScale : { 1.0, 0.0, -1.0, 0.01 })
		{
			for (double MassKg : { 1.0, 2.0 })
			{
				for (double Dt : { 0.005, 0.0025 })
				{
					TUniquePtr<FPreviewScene> Scene = MakeScene();
					UWorld* World = Scene->GetWorld();
					UStaticMeshComponent* Cube = World ? MakeCube(*World, MassKg) : nullptr;
					FBodyInstance* Body = Cube ? Cube->GetBodyInstance() : nullptr;
					if (!Body || !Body->IsValidBodyInstance())
					{
						Test.AddError(TEXT("analytic cube fixture has no physical body"));
						return false;
					}
					// Complete creation before taking the initial condition.
					World->Tick(ELevelTick::LEVELTICK_All, static_cast<float>(Dt));
					++GFrameCounter;
					const double InertiaSi = MassKg * 0.2 * 0.2 / 6.0;
					Test.TestTrue(TEXT("fixture mass matches declared kg"),
						FMath::Abs(Body->GetBodyMass() - MassKg) < 1.0e-5);
					Test.TestTrue(TEXT("uniform cube inertia matches m L^2 / 6 in kg m^2"),
						Matches(Body->GetBodyInertiaTensor() / 10000.0, FVector(InertiaSi), 1.0e-5));
					const FVector Axis = FVector(1.0, 2.0, -1.0).GetSafeNormal();
					const double InputSi = Angular ? 0.02 : 1.0;
					const FVector AccelerationSi = Axis * (InputSi / (Angular ? InertiaSi : MassKg));
					constexpr double Duration = 0.2;
					const int32 Steps = FMath::RoundToInt(Duration / Dt);
					const FTransform Start = Body->GetUnrealWorldTransform();
					for (int32 Step = 0; Step < Steps; ++Step)
					{
						// Exercise the production SI conversion. The oracle above does
						// not call this function or read a converted command back.
						const double ChaosInput = ConvertGenericJointEffortToChaos(InputSi * InputScale,
							Angular ? EUERLJointCoordinate::Twist : EUERLJointCoordinate::LinearX);
						if (Angular) { Cube->AddTorqueInRadians(Axis * ChaosInput, NAME_None, false); }
						else { Cube->AddForce(Axis * ChaosInput, NAME_None, false); }
						if (!Tick(Test, *World, Dt)) { return false; }
					}
					Test.TestTrue(TEXT("analytic body remains awake after applied input"), Body->IsInstanceAwake());
					const FVector Velocity = Angular ? Body->GetUnrealWorldAngularVelocityInRadians()
						: Body->GetUnrealWorldVelocity() / 100.0;
					const FVector ExpectedVelocity = AccelerationSi * Duration;
					// 0.5% relative plus a small absolute floor detects far smaller
					// errors than the former constrained 0.1x..1x acceleration bound.
					const double VelocityTolerance = 1.0e-4 + ExpectedVelocity.Size() * 0.005;
					const bool VelocityMatches = Matches(Velocity, ExpectedVelocity, VelocityTolerance);
					Test.TestEqual(TEXT("velocity oracle accepts nominal input and rejects each corrupted input"),
						VelocityMatches, InputScale == 1.0);
					if (InputScale == 1.0)
					{
						const FTransform End = Body->GetUnrealWorldTransform();
						const double Displacement = Angular
							? (End.GetRotation() * Start.GetRotation().Inverse()).GetNormalized().GetTwistAngle(Axis)
							: FVector::DotProduct((End.GetLocation() - Start.GetLocation()) / 100.0, Axis);
						const double ExpectedDisplacement = 0.5 * AccelerationSi.Size() * Duration * Duration;
						// First-order integration bound, fixed before measurement and
						// proportional to dt. Compare with the continuous solution.
						const double PositionTolerance = 1.0e-4 + AccelerationSi.Size() * Duration * Dt;
						Test.TestTrue(TEXT("displacement stays inside the declared integration-error bound"),
							FMath::Abs(Displacement - ExpectedDisplacement) <= PositionTolerance);
						for (int32 Step = 0; Step < Steps; ++Step)
						{
							if (!Tick(Test, *World, Dt)) { return false; }
						}
						const FVector Coasting = Angular ? Body->GetUnrealWorldAngularVelocityInRadians()
							: Body->GetUnrealWorldVelocity() / 100.0;
						Test.TestTrue(TEXT("zero subsequent input clears applied force and preserves momentum"),
							Matches(Coasting, ExpectedVelocity, VelocityTolerance));
					}
					Test.AddInfo(FString::Printf(
						TEXT("[PHYSICS_ORACLE] angular=%d mass=%.3f dt=%.6f input_scale=%.3f expected_speed=%.7f measured=(%.7f,%.7f,%.7f) tolerance=%.7f accepted=%d"),
						Angular, MassKg, Dt, InputScale, ExpectedVelocity.Size(), Velocity.X, Velocity.Y, Velocity.Z,
						VelocityTolerance, VelocityMatches));
				}
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLAnalyticForceResponseTest,
	"UERL.Integration.PhysicsResponse.FreeBody.ForceMassTrajectoryAndOracleSensitivity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLAnalyticForceResponseTest::RunTest(const FString& Parameters)
{
	return UERLPhysicsResponseTests::RunFreeBody(*this, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLAnalyticTorqueResponseTest,
	"UERL.Integration.PhysicsResponse.FreeBody.TorqueInertiaTrajectoryAndOracleSensitivity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLAnalyticTorqueResponseTest::RunTest(const FString& Parameters)
{
	return UERLPhysicsResponseTests::RunFreeBody(*this, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLLoadedPositionDriveResponseTest,
	"UERL.Integration.PhysicsResponse.Joint.LoadedPositionDriveEquilibrium",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLLoadedPositionDriveResponseTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double Mass : { 1.0, 2.0 })
	{
		for (double LoadNm : { -0.05, 0.05 })
		{
			TUniquePtr<FPreviewScene> Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			UStaticMeshComponent* Cube = World ? MakeCube(*World, Mass) : nullptr;
			if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing hinge body")); return false; }
			UPhysicsConstraintComponent* Joint = PinCube(*Cube, 2.0, 0.2, 0.2);
			constexpr double Target = 0.15;
			constexpr double Dt = 0.005;
			Joint->ConstraintInstance.SetAngularOrientationTarget(FQuat(FVector::XAxisVector, Target));
			double PreviousQ = 0.0;
			double LastQ = 0.0;
			double MaxRestVelocity = 0.0;
			double MaxRestFiniteDifference = 0.0;
			for (int32 Step = 0; Step < 800; ++Step)
			{
				Cube->AddTorqueInRadians(FVector::XAxisVector * (LoadNm * 10000.0), NAME_None, false);
				if (!Tick(*this, *World, Dt)) { return false; }
				LastQ = JointPosition(*Cube, Joint->ConstraintInstance);
				if (Step >= 700)
				{
					MaxRestVelocity = FMath::Max(MaxRestVelocity,
						Cube->GetBodyInstance()->GetUnrealWorldAngularVelocityInRadians().Size());
					MaxRestFiniteDifference = FMath::Max(MaxRestFiniteDifference, FMath::Abs(LastQ - PreviousQ) / Dt);
				}
				PreviousQ = LastQ;
				if (Step == 0 || Step == 799)
				{
					ReportHinge(*this, *Cube, Joint->ConstraintInstance, TEXT("equilibrium"), Step);
				}
			}
			// Static balance: Kp * (target - q) + external_torque = 0.
			const double ExpectedQ = Target + LoadNm / 2.0;
			TestTrue(TEXT("measured load deflection agrees with independent static torque balance"),
				FMath::IsFinite(LastQ) && FMath::Abs(LastQ - ExpectedQ) < 0.0025);
			TestTrue(TEXT("settled orientation and measured angular velocity both approach rest"),
				MaxRestVelocity < 0.005 && MaxRestFiniteDifference < 0.005);
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] equilibrium mass=%.2f load_nm=%.5f expected_q=%.7f q=%.7f max_rest_w=%.7f max_rest_fd=%.7f"),
				Mass, LoadNm, ExpectedQ, LastQ, MaxRestVelocity, MaxRestFiniteDifference));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLSaturatedPositionDriveResponseTest,
	"UERL.Integration.PhysicsResponse.Joint.PositionDriveTorqueSaturation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSaturatedPositionDriveResponseTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double Sign : { -1.0, 1.0 })
	{
		TUniquePtr<FPreviewScene> Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		UStaticMeshComponent* Cube = World ? MakeCube(*World, 10.0) : nullptr;
		if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing saturation body")); return false; }
		UPhysicsConstraintComponent* Joint = PinCube(*Cube, 10.0, 1.0, 0.2);
		Joint->ConstraintInstance.SetAngularOrientationTarget(FQuat::Identity);
		constexpr double Dt = 0.005;
		if (!Tick(*this, *World, Dt)) { return false; }
		FBodyInstance* Body = Cube->GetBodyInstance();
		FTransform Pose = Body->GetUnrealWorldTransform();
		Pose.SetRotation(FQuat(FVector::XAxisVector, Sign * 0.5) * Pose.GetRotation());
		Body->SetBodyTransform(Pose, ETeleportType::TeleportPhysics, true);
		Body->SetAngularVelocityInRadians(FVector::ZeroVector, false);
		Body->SetLinearVelocity(FVector::ZeroVector, false);
		TestTrue(TEXT("saturation trial starts with the declared angular displacement"),
			FMath::Abs(JointPosition(*Cube, Joint->ConstraintInstance) - Sign * 0.5) < 1.0e-4);
		for (int32 Step = 0; Step < 20; ++Step)
		{
			// The drive must oppose the external load at its 0.2 Nm cap.
			Cube->AddTorqueInRadians(FVector::XAxisVector * (Sign * 0.4 * 10000.0), NAME_None, false);
			if (!Tick(*this, *World, Dt)) { return false; }
			ReportHinge(*this, *Cube, Joint->ConstraintInstance, TEXT("saturation"), Step);
		}
		constexpr double InertiaSi = 10.0 * 0.2 * 0.2 / 6.0;
		const double ExpectedW = Sign * (0.4 - 0.2) / InertiaSi * (20 * Dt);
		const FVector ActualW = Body->GetUnrealWorldAngularVelocityInRadians();
		TestTrue(TEXT("measured acceleration exposes the actual saturated drive torque"),
			Matches(ActualW, FVector::XAxisVector * ExpectedW, 0.001 + FMath::Abs(ExpectedW) * 0.02));
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] saturation expected_w=%.7f measured=(%.7f,%.7f,%.7f)"),
			ExpectedW, ActualW.X, ActualW.Y, ActualW.Z));
	}
	return true;
}

#endif

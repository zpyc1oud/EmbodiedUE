#pragma once

#include "Misc/AutomationTest.h"
#include "Chaos/ChaosEngineInterface.h"
#include "Chaos/RigidParticles.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
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

	inline TUniquePtr<FPreviewScene> MakeScene()
	{
		return MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false).SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true).SetTransactional(false).SetEditor(false));
	}

	inline UStaticMeshComponent* MakeCube(UWorld& World, double MassKg, const FVector& SizeMeters = FVector(0.2))
	{
		AActor* Owner = World.SpawnActor<AActor>();
		if (!Owner) { return nullptr; }
		UStaticMeshComponent* Cube = NewObject<UStaticMeshComponent>(Owner);
		Cube->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Cube->SetMobility(EComponentMobility::Movable);
		// The engine cube is 100 cm wide. Scale each axis by its declared length in metres.
		Cube->SetWorldScale3D(SizeMeters);
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
			// The analytical oracle uses the declared geometric inertia. Engine
			// inertia conditioning must not silently replace it in joint solves.
			Body->SetInertiaConditioningEnabled(false);
			if (FPhysicsActorHandle Handle = Body->GetPhysicsActorHandle())
			{
				Handle->GetGameThreadAPI().SetSleepType(Chaos::ESleepType::NeverSleep);
			}
		}
		Cube->WakeAllRigidBodies();
		return Cube;
	}

	inline bool Tick(FAutomationTestBase& Test, UWorld& World, double Dt)
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

	inline bool Matches(const FVector& Actual, const FVector& Expected, double AbsoluteTolerance)
	{
		return !Actual.ContainsNaN() && (Actual - Expected).Size() <= AbsoluteTolerance;
	}

	inline UPhysicsConstraintComponent* PinCube(UStaticMeshComponent& Cube, double Kp, double Kd, double Limit)
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

	inline double JointPosition(UStaticMeshComponent& Cube, FConstraintInstance& Joint)
	{
		return MeasureGenericJointPosition(
			Joint.GetRefFrame(EConstraintFrame::Frame1) * Cube.GetBodyInstance()->GetUnrealWorldTransform(),
			Joint.GetRefFrame(EConstraintFrame::Frame2), EUERLJointCoordinate::Twist);
	}


	inline void ReportHinge(FAutomationTestBase& Test, UStaticMeshComponent& Cube,
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
		const IConsoleVariable* StiffnessScale = IConsoleManager::Get().FindConsoleVariable(
			TEXT("p.Chaos.JointConstraint.AngularDriveStiffnessScale"));
		const IConsoleVariable* DampingScale = IConsoleManager::Get().FindConsoleVariable(
			TEXT("p.Chaos.JointConstraint.AngularDriveDampingScale"));
		Test.AddInfo(FString::Printf(TEXT("[PHYSICS_DIAGNOSTIC] trial=%s step=%d stiffness_scale=%.9f damping_scale=%.9f joint_mass_conditioning=%d"),
			Trial, Step, StiffnessScale ? StiffnessScale->GetFloat() : -1.0f,
			DampingScale ? DampingScale->GetFloat() : -1.0f, Joint.ProfileInstance.bEnableMassConditioning));
		Test.AddInfo(FString::Printf(
			TEXT("[PHYSICS_DIAGNOSTIC] trial=%s step=%d mass=%.9f I_si=(%.9f,%.9f,%.9f) q=%.9f w=(%.9f,%.9f,%.9f) constraint_angular_impulse_nms=(%.9f,%.9f,%.9f) kp_si=%.9f kd_si=%.9f limit_nm=%.9f acceleration=%d projection=%d profile_target_rev_s=(%.9f,%.9f,%.9f) solver_target_rad_s=(%.9f,%.9f,%.9f)"),
			Trial, Step, Body->GetBodyMass(), I.X, I.Y, I.Z, JointPosition(Cube, Joint),
			W.X, W.Y, W.Z, ReactionTorque.X / 10000.0, ReactionTorque.Y / 10000.0, ReactionTorque.Z / 10000.0,
			Drive.TwistDrive.Stiffness / 10000.0, Drive.TwistDrive.Damping / 10000.0,
			Drive.TwistDrive.MaxForce / 10000.0, Drive.bAccelerationMode, Joint.IsProjectionEnabled(),
			ProfileTarget.X, ProfileTarget.Y, ProfileTarget.Z, SolverTarget.X, SolverTarget.Y, SolverTarget.Z));
	}

}

#endif

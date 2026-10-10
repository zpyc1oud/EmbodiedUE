#include "UERLPhysicsResponseTestSupport.h"
#include "UERLContactWrenchTestSupport.h"
#include "UERLAuthoredMassTestSupport.h"

#include "Components/SkeletalMeshComponent.h"
#include "Chaos/ChaosConstraintSettings.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Collision/ParticleCollisions.h"
#include "Chaos/ParticleHandle.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "UERLSkeletalMeshRobotRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UERLPhantomXPhysicsTests
{
	FUERLSkeletalMeshRobotRuntimeConfig Config(double Height = 0.18)
	{
		FUERLSkeletalMeshRobotRuntimeConfig Result;
		Result.AssetPath = TEXT("/Game/Robots/PhantomX/SK_PhantomX.SK_PhantomX");
		Result.bClaimAuthoredActor = false;
		Result.InitialRootHeightMeters = Height;
		for (const TCHAR* Leg : { TEXT("rf"), TEXT("rm"), TEXT("rr"), TEXT("lf"), TEXT("lm"), TEXT("lr") })
		{
			for (const TPair<const TCHAR*, double>& Segment : {
				TPair<const TCHAR*, double>(TEXT("c1"), 0.0),
				TPair<const TCHAR*, double>(TEXT("thigh"), 0.15),
				TPair<const TCHAR*, double>(TEXT("tibia"), -0.30) })
			{
				FUERLSkeletalMeshRuntimeActuator& A = Result.Actuators.AddDefaulted_GetRef();
				A.JointName = FName(*FString::Printf(TEXT("%s_%s"), Segment.Key, Leg));
				A.Stiffness = 25.0;
				A.Damping = 0.5;
				A.EffortLimit = 2.8;
				A.DefaultPosition = Segment.Value;
				Result.Observations.Add({ EUERLObservationType::JointPosition, A.JointName });
				Result.Observations.Add({ EUERLObservationType::JointVelocity, A.JointName });
			}
		}
		return Result;
	}

	USkeletalMeshComponent* FindMesh(UWorld& World)
	{
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			USkeletalMeshComponent* Mesh = It->FindComponentByClass<USkeletalMeshComponent>();
			if (Mesh && Mesh->GetFName() == FName(TEXT("RobotSkeletalMesh"))) { return Mesh; }
		}
		return nullptr;
	}

	bool Ground(UWorld& World)
	{
		AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>();
		if (!Actor) { return false; }
		UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
		Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetWorldScale3D(FVector(20.0, 20.0, 0.2));
		Mesh->SetWorldLocation(FVector(0.0, 0.0, -10.0));
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Mesh->SetCollisionObjectType(ECC_WorldStatic);
		Mesh->SetCollisionResponseToAllChannels(ECR_Block);
		return Mesh->GetStaticMesh() != nullptr;
	}

	bool SaveTrace(FAutomationTestBase& Test, const FString& Name, const FString& Contents)
	{
		const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/PhysicsResponse"));
		if (!IFileManager::Get().MakeDirectory(*Directory, true))
		{
			Test.AddError(TEXT("cannot create bounded physical trace directory"));
			return false;
		}
		const FString Path = FPaths::Combine(Directory, Name + TEXT("-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".csv"));
		if (!FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test.AddError(TEXT("cannot save physical trace"));
			return false;
		}
		Test.AddInfo(FString::Printf(TEXT("[PHYSICS_TRACE] %s"), *Path));
		return true;
	}

	FString PreciseVector(const FVector& V)
	{
		return FString::Printf(TEXT("(%.9f,%.9f,%.9f)"), V.X, V.Y, V.Z);
	}

	void ReportInitialPose(FAutomationTestBase& Test, USkeletalMeshComponent& Mesh, const TCHAR* Stage)
	{
		FBodyInstance* Root = Mesh.GetBodyInstance(FName(TEXT("base_link")));
		FPhysicsActorHandle Handle = Root ? Root->GetPhysicsActorHandle() : nullptr;
		if (!Handle) { Test.AddError(TEXT("initial-pose diagnostic requires root actor")); return; }
		FPhysicsCommand::ExecuteRead(Handle, [&](const FPhysicsActorHandle& ReadHandle)
		{
			auto* Particle = ReadHandle->GetHandle_LowLevel()
				? ReadHandle->GetHandle_LowLevel()->CastToRigidParticle() : nullptr;
			const auto* KinematicGT = ReadHandle->GetParticle_LowLevel()->CastToKinematicParticle();
			if (KinematicGT)
			{
				const auto Target = KinematicGT->KinematicTarget();
				Test.AddInfo(FString::Printf(TEXT("[PHYSICS_LIFECYCLE] stage=%s gt_state=%d kinematic_mode=%d kinematic_dirty=%d kinematic_position_cm=%s"),
					Stage, static_cast<int32>(ReadHandle->GetGameThreadAPI().ObjectState()),
					static_cast<int32>(Target.GetMode()), ReadHandle->GetGameThreadAPI().IsKinematicTargetDirty(),
					*PreciseVector(Target.GetMode() == Chaos::EKinematicTargetMode::Position ? FVector(Target.GetPosition()) : FVector::ZeroVector)));
			}
			Test.AddInfo(FString::Printf(TEXT("[PHYSICS_LIFECYCLE] stage=%s component_cm=%s body_cm=%s com_cm=%s gt_cm=%s solver_exists=%d solver_cm=%s"),
				Stage, *PreciseVector(Mesh.GetComponentLocation()),
				*PreciseVector(Root->GetUnrealWorldTransform().GetLocation()),
				*PreciseVector(Root->GetCOMPosition()), *PreciseVector(ReadHandle->GetGameThreadAPI().X()),
				Particle != nullptr, *PreciseVector(Particle ? FVector(Particle->X()) : FVector::ZeroVector)));
		});
	}

	void SetDiagnosticConditioning(USkeletalMeshComponent& Mesh, int32 Mode)
	{
		// Product configuration is mode zero. Separate the two conditioning
		// mechanisms without changing projection or acceptance tolerances.
		if (Mode & 1)
		{
			for (FConstraintInstance* C : Mesh.Constraints) { if (C) { C->DisableMassConditioning(); } }
		}
		if (Mode & 2)
		{
			for (FBodyInstance* B : Mesh.Bodies) { if (B) { B->SetInertiaConditioningEnabled(false); } }
		}
	}

	struct FMomentum
	{
		FVector Linear = FVector::ZeroVector;
		FVector Angular = FVector::ZeroVector;
	};

	FMomentum Momentum(USkeletalMeshComponent& Mesh)
	{
		FMomentum Result;
		for (FBodyInstance* Body : Mesh.Bodies)
		{
			if (!Body || !Body->IsValidBodyInstance()) { continue; }
			const double Mass = Body->GetBodyMass();
			const FVector P = Body->GetUnrealWorldVelocity() * (Mass / 100.0);
			const FVector I = Body->GetBodyInertiaTensor() / 10000.0;
			const FQuat MassRotation = Body->GetMassSpaceToWorldSpace().GetRotation();
			const FVector W = Body->GetUnrealWorldAngularVelocityInRadians();
			const FVector Spin = MassRotation.RotateVector(I * MassRotation.UnrotateVector(W));
			Result.Linear += P;
			Result.Angular += Spin + FVector::CrossProduct(Body->GetCOMPosition() / 100.0, P);
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhantomXPhysicalInventoryTest,
	"UERL.Integration.PhysicsResponse.PhantomX.AssetAndLiveParameterInventory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhantomXPhysicalInventoryTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhantomXPhysicsTests;
	FLockstepSettings Settings;
	auto Scene = MakeScene();
	UWorld* World = Scene->GetWorld();
	if (!World || !Ground(*World)) { AddError(TEXT("missing inventory World")); return false; }
	const auto RobotConfig = Config();
	FUERLSkeletalMeshRobotRuntime Runtime;
	FString Error;
	if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
	USkeletalMeshComponent* Mesh = FindMesh(*World);
	UPhysicsAsset* Asset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!Mesh || !Asset) { AddError(TEXT("actual PhantomX PhysicsAsset is required")); return false; }
	TestEqual(TEXT("PhantomX has 19 physical bodies"), Asset->SkeletalBodySetups.Num(), 19);
	TestEqual(TEXT("PhantomX has 18 live constraints"), Mesh->Constraints.Num(), 18);
	TSet<FName> Names;
	double TotalMass = 0.0;
	for (USkeletalBodySetup* Setup : Asset->SkeletalBodySetups)
	{
		if (!Setup) { AddError(TEXT("missing asset body setup")); return false; }
		TestFalse(TEXT("physical body names are unique"), Names.Contains(Setup->BoneName));
		Names.Add(Setup->BoneName);
		FBodyInstance* Body = Mesh->GetBodyInstance(Setup->BoneName);
		if (!Body || !Body->IsValidBodyInstance()) { AddError(TEXT("asset body has no live solver body")); return false; }
		if (!CheckAuthoredMassProperties(*this, *Setup, *Body)) { return false; }
		const double Mass = Body->GetBodyMass();
		TestTrue(TEXT("the floating PhantomX topology simulates every declared body with gravity"),
			Body->IsInstanceSimulatingPhysics() && Body->bEnableGravity);
		TestTrue(TEXT("authored damping and mass scale reach each body instance"),
			FMath::Abs(Body->LinearDamping - Setup->DefaultInstance.LinearDamping) < 1.0e-6
			&& FMath::Abs(Body->AngularDamping - Setup->DefaultInstance.AngularDamping) < 1.0e-6
			&& FMath::Abs(Body->MassScale - Setup->DefaultInstance.MassScale) < 1.0e-6);
		if (Setup->DefaultInstance.bOverrideMass)
		{
			TestTrue(TEXT("authored mass override reaches the live body"),
				FMath::Abs(Mass - Setup->DefaultInstance.GetMassOverride()) <= 1.0e-5);
		}
		else
		{
			// Compare the authored geometry/material calculation with the live
			// solver body. This is a configuration-transfer check, separate from
			// the analytical force/inertia response fixtures.
			const double AuthoredMass = Setup->CalculateMass(Mesh);
			TestTrue(TEXT("authored computed mass reaches the live body"),
				FMath::IsFinite(AuthoredMass) && AuthoredMass > 0.0
				&& FMath::Abs(Mass - AuthoredMass) <= 1.0e-5 + 0.005 * AuthoredMass);
			AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] body=%s computed_mass_kg=%.9f live_mass_kg=%.9f"),
				*Setup->BoneName.ToString(), AuthoredMass, Mass));
		}
		TestTrue(TEXT("authored COM nudge and inertia scaling reach the body instance"),
			Body->COMNudge.Equals(Setup->DefaultInstance.COMNudge, 1.0e-6)
			&& Body->InertiaTensorScale.Equals(Setup->DefaultInstance.InertiaTensorScale, 1.0e-6));
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] body=%s authored_mass_override=%d authored_mass_kg=%.9f authored_inertia_scale=%s"),
			*Setup->BoneName.ToString(), Setup->DefaultInstance.bOverrideMass,
			Setup->DefaultInstance.GetMassOverride(), *Setup->DefaultInstance.InertiaTensorScale.ToString()));
		const FVector I = Body->GetBodyInertiaTensor() / 10000.0;
		TestTrue(TEXT("mass and principal inertia are positive and finite"),
			FMath::IsFinite(Mass) && Mass > 0.0 && !I.ContainsNaN() && I.GetMin() > 0.0);
		// A realizable rigid-body inertia has principal moments satisfying the triangle inequalities.
		TestTrue(TEXT("principal inertias satisfy rigid-body triangle inequalities"),
			I.X <= I.Y + I.Z + 1.0e-8 && I.Y <= I.X + I.Z + 1.0e-8 && I.Z <= I.X + I.Y + 1.0e-8);
		const FVector ExtentMeters = Body->GetBodyBounds().GetSize() / 100.0;
		TestTrue(TEXT("collision dimensions use the expected metre scale"),
			!ExtentMeters.ContainsNaN() && ExtentMeters.GetMin() > 0.001 && ExtentMeters.GetMax() < 1.0);
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] body=%s simulated=%d gravity=%d linear_damping=%.9f angular_damping=%.9f"),
			*Setup->BoneName.ToString(), Body->IsInstanceSimulatingPhysics(), Body->bEnableGravity,
			Body->LinearDamping, Body->AngularDamping));
		TotalMass += Mass;
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] body=%s mass_kg=%.9f inertia_kg_m2=%s com_cm=%s mass_frame=%s bounds_cm=%s"),
			*Setup->BoneName.ToString(), Mass, *I.ToString(), *Body->GetCOMPosition().ToString(),
			*Body->GetMassSpaceLocal().ToString(), *Body->GetBodyBounds().ToString()));
	}
	TSet<FName> Joints;
	for (FConstraintInstance* C : Mesh->Constraints)
	{
		if (!C) { AddError(TEXT("missing live joint")); return false; }
		TestFalse(TEXT("joint names are unique"), Joints.Contains(C->JointName));
		Joints.Add(C->JointName);
		UPhysicsConstraintTemplate* Authored = nullptr;
		for (UPhysicsConstraintTemplate* Candidate : Asset->ConstraintSetup)
		{
			if (Candidate && Candidate->DefaultInstance.JointName == C->JointName) { Authored = Candidate; break; }
		}
		if (!Authored) { AddError(TEXT("live joint has no authored constraint")); return false; }
		FConstraintInstance& Original = Authored->DefaultInstance;
		TestTrue(TEXT("authored joint endpoints reach the live constraint"),
			Original.ConstraintBone1 == C->ConstraintBone1 && Original.ConstraintBone2 == C->ConstraintBone2);
		TestTrue(TEXT("authored free and locked coordinates reach the live constraint"),
			Original.GetAngularTwistMotion() == C->GetAngularTwistMotion()
			&& Original.GetAngularSwing1Motion() == C->GetAngularSwing1Motion()
			&& Original.GetAngularSwing2Motion() == C->GetAngularSwing2Motion()
			&& Original.GetLinearXMotion() == C->GetLinearXMotion()
			&& Original.GetLinearYMotion() == C->GetLinearYMotion()
			&& Original.GetLinearZMotion() == C->GetLinearZMotion());
		TestTrue(TEXT("authored angular limits reach the live constraint"),
			FMath::Abs(Original.GetAngularTwistLimit() - C->GetAngularTwistLimit()) < 1.0e-5
			&& FMath::Abs(Original.GetAngularSwing1Limit() - C->GetAngularSwing1Limit()) < 1.0e-5
			&& FMath::Abs(Original.GetAngularSwing2Limit() - C->GetAngularSwing2Limit()) < 1.0e-5);
		for (const EConstraintFrame::Type Frame : { EConstraintFrame::Frame1, EConstraintFrame::Frame2 })
		{
			TestTrue(TEXT("authored constraint reference frames reach the live constraint"),
				Original.GetRefFrame(Frame).Equals(C->GetRefFrame(Frame), 1.0e-5));
		}
		TestTrue(TEXT("authored soft-limit selection reaches the live constraint"),
			Original.ProfileInstance.LinearLimit.bSoftConstraint == C->ProfileInstance.LinearLimit.bSoftConstraint
			&& Original.ProfileInstance.ConeLimit.bSoftConstraint == C->ProfileInstance.ConeLimit.bSoftConstraint
			&& Original.ProfileInstance.TwistLimit.bSoftConstraint == C->ProfileInstance.TwistLimit.bSoftConstraint);
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] joint=%s soft_linear=%d soft_swing=%d soft_twist=%d soft_linear_k=%.9f soft_swing_k=%.9f soft_twist_k=%.9f"),
			*C->JointName.ToString(), C->ProfileInstance.LinearLimit.bSoftConstraint,
			C->ProfileInstance.ConeLimit.bSoftConstraint, C->ProfileInstance.TwistLimit.bSoftConstraint,
			C->GetSoftLinearLimitStiffness(), C->GetSoftSwingLimitStiffness(), C->GetSoftTwistLimitStiffness()));
		TestTrue(TEXT("each constraint refers to physical bodies"), Names.Contains(C->ConstraintBone1) && Names.Contains(C->ConstraintBone2));
		TestTrue(TEXT("each live joint has a declared actuator"), RobotConfig.Actuators.ContainsByPredicate(
			[C](const FUERLSkeletalMeshRuntimeActuator& A) { return A.JointName == C->JointName; }));
		const auto& Drive = C->ProfileInstance.AngularDrive;
		const auto& AxisDrive = C->GetAngularTwistMotion() != EAngularConstraintMotion::ACM_Locked
			? Drive.TwistDrive : Drive.SwingDrive;
		const double ActualKp = AxisDrive.Stiffness * Chaos::ConstraintSettings::AngularDriveStiffnessScale() / 10000.0;
		const double ActualKd = AxisDrive.Damping * Chaos::ConstraintSettings::AngularDriveDampingScale() / 10000.0;
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] joint=%s motions_xyz=%d,%d,%d swing1=%d swing2=%d limits_deg=%.6f,%.6f,%.6f"),
			*C->JointName.ToString(), int32(C->GetLinearXMotion()), int32(C->GetLinearYMotion()), int32(C->GetLinearZMotion()),
			int32(C->GetAngularSwing1Motion()), int32(C->GetAngularSwing2Motion()), C->GetAngularTwistLimit(),
			C->GetAngularSwing1Limit(), C->GetAngularSwing2Limit()));
		TestTrue(TEXT("declared SI stiffness reaches every PhantomX joint drive"), FMath::Abs(ActualKp - 25.0) < 1.0e-4);
		TestTrue(TEXT("declared SI damping reaches every PhantomX joint drive"), FMath::Abs(ActualKd - 0.5) < 1.0e-5);
		TestTrue(TEXT("every live drive uses the declared force-mode torque cap"),
			!Drive.bAccelerationMode && FMath::Abs(AxisDrive.MaxForce / 10000.0 - 2.8) < 1.0e-5);
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] joint=%s effective_kp_nm_rad=%.9f effective_kd_nm_s_rad=%.9f cap_nm=%.9f target=%s"),
			*C->JointName.ToString(), ActualKp, ActualKd, AxisDrive.MaxForce / 10000.0, *Drive.OrientationTarget.ToString()));
		FVector SolverTarget;
		FPhysicsInterface::GetDriveAngularVelocity(C->GetPhysicsConstraintRef(), SolverTarget);
		TestTrue(TEXT("zero requested drive speed reaches the solver"), SolverTarget.Size() < 1.0e-8);
		TestFalse(TEXT("Robot physics disables position-only joint projection"), C->IsProjectionEnabled());
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] joint=%s child=%s parent=%s twist_motion=%d twist_limit_deg=%.6f projection=%d mass_conditioning=%d frame1=%s frame2=%s"),
			*C->JointName.ToString(), *C->ConstraintBone1.ToString(), *C->ConstraintBone2.ToString(),
			static_cast<int32>(C->GetAngularTwistMotion()), C->GetAngularTwistLimit(), C->IsProjectionEnabled(),
			C->ProfileInstance.bEnableMassConditioning, *C->GetRefFrame(EConstraintFrame::Frame1).ToString(),
			*C->GetRefFrame(EConstraintFrame::Frame2).ToString()));
	}
	AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] total_mass_kg=%.9f; model=declared_idealized_drive"), TotalMass));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhantomXStateConsistencyTest,
	"UERL.Integration.PhysicsResponse.PhantomX.FixedTargetPositionVelocityConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhantomXStateConsistencyTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhantomXPhysicsTests;
	FLockstepSettings Settings;
	TArray<float> D1Final;
	for (int32 Mode : { 0, 1, 2, 3 })
	for (int32 Decimation : { 1, 4 })
	{
		if (Mode != 0 && Decimation != 1) { continue; }
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		if (!World || !Ground(*World)) { AddError(TEXT("missing state-consistency World")); return false; }
		const auto RobotConfig = Config();
		FUERLSkeletalMeshRobotRuntime Runtime;
		FString Error;
		if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
		USkeletalMeshComponent* Mesh = FindMesh(*World);
		if (!Mesh) { AddError(TEXT("missing consistency mesh")); return false; }
		SetDiagnosticConditioning(*Mesh, Mode);
		TArray<float> Targets;
		for (const auto& A : RobotConfig.Actuators) { Targets.Add(A.DefaultPosition); }
		TArray<double> DeltaQ, IntegratedW, Variation, MaximumPrefixError, MaximumPrefixExcess;
		DeltaQ.Init(0.0, 18); IntegratedW.Init(0.0, 18); Variation.Init(0.0, 18);
		MaximumPrefixError.Init(0.0, 18); MaximumPrefixExcess.Init(-0.008, 18);
		TArray<float> Previous, State;
		if (!Runtime.CollectState(Previous, Error) || Previous.Num() != 36) { AddError(TEXT("expected 36 named joint fields")); return false; }
		FString Trace = TEXT("solver_step,solver_frame,solver_time_s,solver_dt_s,decimation,joint,target_rad,q_rad,qd_rad_s\n");
		constexpr double Dt = 0.005;
		for (int32 Step = 0; Step < 1600; ++Step)
		{
			if (Step % Decimation == 0 && !Runtime.ApplyActuatorTargets(Targets, Error)) { AddError(Error); return false; }
			if (!Tick(*this, *World, Dt) || !Runtime.CollectState(State, Error)) { AddError(Error); return false; }
			if (State.Num() != 36) { AddError(TEXT("joint state width changed")); return false; }
			FUERLSolverClockSnapshot Clock;
			if (!Runtime.ReadCompletedSolverClock(Clock, Error)) { AddError(Error); return false; }
			for (int32 J = 0; J < 18; ++J)
			{
				const double Q = State[2 * J], W = State[2 * J + 1];
				if (!FMath::IsFinite(Q) || !FMath::IsFinite(W)) { AddError(TEXT("nonfinite physical state")); return false; }
				if (Step >= 400)
				{
					const double Difference = Q - Previous[2 * J];
					DeltaQ[J] += FMath::Atan2(FMath::Sin(Difference), FMath::Cos(Difference));
					IntegratedW[J] += 0.5 * (W + Previous[2 * J + 1]) * Dt;
					Variation[J] += FMath::Abs(W - Previous[2 * J + 1]);
					const double PrefixError = FMath::Abs(DeltaQ[J] - IntegratedW[J]);
					MaximumPrefixError[J] = FMath::Max(MaximumPrefixError[J], PrefixError);
					MaximumPrefixExcess[J] = FMath::Max(MaximumPrefixExcess[J],
						PrefixError - (0.008 + 0.5 * Dt * Variation[J]));
				}
				Trace += FString::Printf(TEXT("%d,%lld,%.9f,%.9f,%d,%s,%.9f,%.9f,%.9f\n"), Step + 1,
					static_cast<long long>(Clock.Frame), Clock.SolverTime, Clock.LastDt, Decimation, *RobotConfig.Actuators[J].JointName.ToString(), Targets[J], Q, W);
			}
			Previous = State;
		}
		if (!SaveTrace(*this, FString::Printf(TEXT("phantomx-conditioning%d-D%d"), Mode, Decimation), Trace)) { return false; }
		for (int32 J = 0; J < 18; ++J)
		{
			// Include a first-order variation budget and an absolute six-second
			// drift allowance. A persistent stationary q/nonzero qdot cannot pass.
			const double Tolerance = 0.008 + 0.5 * Dt * Variation[J];
			const FString Label = FString::Printf(TEXT("%s D%d position change agrees with solver-step velocity integral"),
				*RobotConfig.Actuators[J].JointName.ToString(), Decimation);
			// A later opposite error or noisy section cannot erase an earlier failure.
			if (Mode == 0) { TestTrue(*Label, MaximumPrefixExcess[J] <= 0.0); }
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] conditioning=%d joint=%s D=%d delta_q=%.9f integral_qd=%.9f tolerance=%.9f max_prefix_error=%.9f max_prefix_excess=%.9f"),
				Mode, *RobotConfig.Actuators[J].JointName.ToString(), Decimation, DeltaQ[J], IntegratedW[J], Tolerance,
				MaximumPrefixError[J], MaximumPrefixExcess[J]));
		}
		if (Decimation == 1) { D1Final = State; }
		else
		{
			for (int32 Index = 0; Index < State.Num(); ++Index)
			{
				TestTrue(TEXT("fixed-target terminal state is consistent across control grouping"),
					FMath::Abs(State[Index] - D1Final[Index]) < (Index % 2 == 0 ? 0.001 : 0.01));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhantomXMomentumTest,
	"UERL.Integration.PhysicsResponse.PhantomX.TotalMomentumUnderExternalImpulse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhantomXMomentumTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhantomXPhysicsTests;
	FLockstepSettings Settings;
	for (int32 Mode : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 })
	for (double Sign : { -1.0, 0.0, 1.0 })
	{
		if (Mode >= 7 && Mode != 10 && Sign != 0.0) { continue; }
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		if (!World) { return false; }
		if ((Mode == 4 || Mode == 5) && !Tick(*this, *World, 0.005)) { return false; }
		const auto RobotConfig = Config(3.0);
		FUERLSkeletalMeshRobotRuntime Runtime;
		FString Error;
		if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
		USkeletalMeshComponent* Mesh = FindMesh(*World);
		if (!Mesh) { AddError(TEXT("momentum test requires the actual PhantomX")); return false; }
		const bool bReportLifecycle = Sign == 0.0 && (Mode == 0 || Mode >= 6);
		if (bReportLifecycle) { ReportInitialPose(*this, *Mesh, TEXT("initialized")); }
		Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
		for (FBodyInstance* Body : Mesh->Bodies)
		{
			if (!Body || !Body->IsValidBodyInstance()) { AddError(TEXT("missing momentum body")); return false; }
			Body->SetEnableGravity(false);
			Body->LinearDamping = 0.0f;
			Body->AngularDamping = 0.0f;
			Body->UpdateDampingProperties();
		}
		SetDiagnosticConditioning(*Mesh, Mode == 5 ? 2 : Mode >= 4 ? 0 : Mode);
		if (bReportLifecycle) { ReportInitialPose(*this, *Mesh, TEXT("configured")); }
		const FVector InitialCOM = Mesh->GetBodyInstance(FName(TEXT("base_link")))->GetCOMPosition();
		TArray<FTransform> InitialTransforms;
		TArray<FVector> InitialLinearVelocities, InitialAngularVelocities;
		if (Mode == 6 || Mode == 8)
		{
			for (FBodyInstance* Body : Mesh->Bodies)
			{
				InitialTransforms.Add(Body->GetUnrealWorldTransform());
				InitialLinearVelocities.Add(Body->GetUnrealWorldVelocity());
				InitialAngularVelocities.Add(Body->GetUnrealWorldAngularVelocityInRadians());
			}
		}
		const auto ReplayInitialBodies = [&]()
		{
			for (int32 Index = 0; Index < Mesh->Bodies.Num(); ++Index)
			{
				FBodyInstance* Body = Mesh->Bodies[Index];
				Body->SetBodyTransform(InitialTransforms[Index], ETeleportType::TeleportPhysics, false);
				Body->SetLinearVelocity(InitialLinearVelocities[Index], false);
				Body->SetAngularVelocityInRadians(InitialAngularVelocities[Index], false);
			}
		};
		TArray<float> Targets;
		for (const auto& A : RobotConfig.Actuators) { Targets.Add(A.DefaultPosition); }
		if (!Runtime.ApplyActuatorTargets(Targets, Error)) { AddError(Error); return false; }
		if (bReportLifecycle) { ReportInitialPose(*this, *Mesh, TEXT("targets_applied")); }
		if (Mode == 7)
		{
			for (FBodyInstance* Body : Mesh->Bodies)
			{
				FPhysicsCommand::ExecuteWrite(Body->GetPhysicsActorHandle(), [](const FPhysicsActorHandle& Handle)
				{
					auto& GT = Handle->GetGameThreadAPI();
					GT.SetX(GT.X());
					GT.SetR(GT.R());
				});
			}
			ReportInitialPose(*this, *Mesh, TEXT("same_pose_marked"));
		}
		if (Mode == 8)
		{
			FUERLSolverClockSnapshot BeforeFlush, AfterFlush;
			if (!ReadUERLSolverClock(*World, BeforeFlush, Error)) { AddError(Error); return false; }
			World->GetPhysicsScene()->Flush();
			if (!ReadUERLSolverClock(*World, AfterFlush, Error)) { AddError(Error); return false; }
			AddInfo(FString::Printf(TEXT("[PHYSICS_LIFECYCLE] flush_delta_frame=%lld flush_delta_time=%.9f"),
				static_cast<long long>(AfterFlush.Frame - BeforeFlush.Frame), AfterFlush.SolverTime - BeforeFlush.SolverTime));
			ReportInitialPose(*this, *Mesh, TEXT("zero_dt_flush"));
			ReplayInitialBodies();
			ReportInitialPose(*this, *Mesh, TEXT("flushed_state_replayed"));
		}
		if (Mode == 9) { Mesh->SetComponentTickEnabled(false); }
		if (Mode == 10)
		{
			for (FBodyInstance* Body : Mesh->Bodies)
			{
				FPhysicsCommand::ExecuteWrite(Body->GetPhysicsActorHandle(), [](const FPhysicsActorHandle& Handle)
				{
					Handle->GetGameThreadAPI().SetKinematicTarget(Chaos::FKinematicTarget());
				});
			}
			ReportInitialPose(*this, *Mesh, TEXT("kinematic_target_cleared"));
		}
		if (!Tick(*this, *World, 0.005)) { return false; }
		if (bReportLifecycle) { ReportInitialPose(*this, *Mesh, TEXT("first_tick")); }
		if (Mode == 6)
		{
			// Diagnostic only: repeat the exact pre-step state after registration.
			// The formal mode-zero acceptance remains unchanged.
			ReplayInitialBodies();
			if (bReportLifecycle) { ReportInitialPose(*this, *Mesh, TEXT("state_replayed")); }
			if (!Tick(*this, *World, 0.005)) { return false; }
			if (bReportLifecycle) { ReportInitialPose(*this, *Mesh, TEXT("replay_tick")); }
		}
		FBodyInstance* Root = Mesh->GetBodyInstance(FName(TEXT("base_link")));
		if (!Root) { AddError(TEXT("missing PhantomX root body")); return false; }
		const FMomentum Before = Momentum(*Mesh);
		const FVector Impulse = Sign * FVector(0.02, -0.01, 0.03);
		const FVector At = Root->GetCOMPosition() / 100.0;
		Mesh->AddImpulseAtLocation(Impulse * 100.0, At * 100.0, FName(TEXT("base_link")));
		for (int32 Step = 0; Step < 20; ++Step)
		{
			if (!Tick(*this, *World, 0.005)) { return false; }
		}
		const FMomentum After = Momentum(*Mesh);
		const FVector ExpectedAngular = FVector::CrossProduct(At, Impulse);
		if (Mode == 0)
		{
		TestTrue(TEXT("internal articulation forces preserve total linear momentum"),
			Matches(After.Linear - Before.Linear, Impulse, 0.001 + Impulse.Size() * 0.02));
		TestTrue(TEXT("internal articulation torques preserve total angular momentum about the fixed world origin"),
			Matches(After.Angular - Before.Angular, ExpectedAngular, 0.0001 + ExpectedAngular.Size() * 0.02));
		}
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] phantomx_momentum conditioning=%d sign=%.0f expected_dp=%s actual_dp=%s expected_dL=%s actual_dL=%s error_dp=%.9f error_dL=%.9f initial_com_cm=%s first_com_m=%s"),
			Mode, Sign, *PreciseVector(Impulse), *PreciseVector(After.Linear - Before.Linear),
			*PreciseVector(ExpectedAngular), *PreciseVector(After.Angular - Before.Angular),
			(After.Linear - Before.Linear - Impulse).Size(), (After.Angular - Before.Angular - ExpectedAngular).Size(),
			*PreciseVector(InitialCOM), *PreciseVector(At)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhantomXNamedRoutingTest,
	"UERL.Integration.PhysicsResponse.PhantomX.NamedRoutingAndIndependentState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhantomXNamedRoutingTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhantomXPhysicsTests;
	FLockstepSettings Settings;
	auto Scene = MakeScene();
	UWorld* World = Scene->GetWorld();
	if (!World || !Ground(*World)) { AddError(TEXT("missing routing World")); return false; }
	const auto Canonical = Config();
	auto Reordered = Canonical;
	for (int32 J = 0; J < 9; ++J) { Reordered.Actuators.Swap(J, 17 - J); }
	FUERLSkeletalMeshRobotRuntime Runtime;
	FString Error;
	if (!Runtime.Initialize(*World, Reordered, Error)) { AddError(Error); return false; }
	USkeletalMeshComponent* Mesh = FindMesh(*World);
	if (!Mesh) { AddError(TEXT("missing routing mesh")); return false; }
	TMap<FName, double> Expected;
	for (int32 J = 0; J < 18; ++J)
	{
		Expected.Add(Canonical.Actuators[J].JointName, Canonical.Actuators[J].DefaultPosition + (J - 8.5) * 0.002);
	}
	TArray<float> Targets;
	for (const auto& A : Reordered.Actuators) { Targets.Add(Expected[A.JointName]); }
	if (!Runtime.ApplyActuatorTargets(Targets, Error)) { AddError(Error); return false; }
	for (int32 Step = 0; Step < 100; ++Step)
	{
		if (!Tick(*this, *World, 0.005)) { return false; }
		TArray<float> State;
		if (!Runtime.CollectState(State, Error) || State.Num() != 36)
		{
			AddError(TEXT("routing capture requires all named joint states: ") + Error);
			return false;
		}
		for (int32 J = 0; J < 18; ++J)
		{
			const FName Name = Canonical.Actuators[J].JointName;
			FConstraintInstance* C = Mesh->FindConstraintInstance(Name);
			if (!C) { AddError(TEXT("named actuator has no physical constraint")); return false; }
			const bool Twist = C->GetAngularTwistMotion() != EAngularConstraintMotion::ACM_Locked;
			const bool Swing1 = C->GetAngularSwing1Motion() != EAngularConstraintMotion::ACM_Locked;
			const bool Swing2 = C->GetAngularSwing2Motion() != EAngularConstraintMotion::ACM_Locked;
			if (!TestTrue(TEXT("the physical joint has exactly one angular coordinate"),
				static_cast<int32>(Twist) + static_cast<int32>(Swing1) + static_cast<int32>(Swing2) == 1)) { return false; }
			// Literal Chaos axis convention; do not use the production axis resolver.
			const FVector LocalAxis = Twist ? FVector::XAxisVector : Swing1 ? FVector::ZAxisVector : FVector::YAxisVector;
			const double Target = C->ProfileInstance.AngularDrive.OrientationTarget.Quaternion().GetTwistAngle(LocalAxis);
			TestTrue(TEXT("reordered named command reaches the intended live drive"), FMath::Abs(Target - Expected[Name]) < 1.0e-6);
			FBodyInstance* Child = Mesh->GetBodyInstance(C->ConstraintBone1);
			FBodyInstance* Parent = Mesh->GetBodyInstance(C->ConstraintBone2);
			if (!Child || !Parent) { AddError(TEXT("missing named joint bodies")); return false; }
			const FTransform ChildFrame = C->GetRefFrame(EConstraintFrame::Frame1) * Child->GetUnrealWorldTransform();
			const FTransform ParentFrame = C->GetRefFrame(EConstraintFrame::Frame2) * Parent->GetUnrealWorldTransform();
			const FQuat Relative = (ParentFrame.GetRotation().Inverse() * ChildFrame.GetRotation()).GetNormalized();
			const double GeometricQ = 2.0 * FMath::Atan2(
				FVector::DotProduct(FVector(Relative.X, Relative.Y, Relative.Z), LocalAxis), Relative.W);
			const FVector Axis = ChildFrame.GetRotation().RotateVector(LocalAxis);
			const double PhysicalW = FVector::DotProduct(Axis,
				Child->GetUnrealWorldAngularVelocityInRadians() - Parent->GetUnrealWorldAngularVelocityInRadians());
			const double Difference = State[J * 2] - GeometricQ;
			TestTrue(TEXT("named position state preserves independently measured geometry"),
				FMath::Abs(FMath::Atan2(FMath::Sin(Difference), FMath::Cos(Difference))) < 1.0e-5);
			TestTrue(TEXT("named velocity state preserves the completed body velocity"),
				FMath::Abs(State[J * 2 + 1] - PhysicalW) < 1.0e-5);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhantomXSupportBalanceTest,
	"UERL.Integration.PhysicsResponse.PhantomX.WholeBodyWeightSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhantomXSupportBalanceTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhantomXPhysicsTests;
	FLockstepSettings Settings;
	auto Scene = MakeScene();
	UWorld* World = Scene->GetWorld();
	if (!World || !Ground(*World)) { AddError(TEXT("missing support ground")); return false; }
	auto RobotConfig = Config();
	TArray<FName> ForceBodies = { FName(TEXT("base_link")) };
	for (const auto& A : RobotConfig.Actuators) { ForceBodies.Add(A.JointName); }
	for (FName Name : ForceBodies) { RobotConfig.Observations.Add({ EUERLObservationType::ContactForce, Name }); }
	FUERLSkeletalMeshRobotRuntime Runtime;
	FString Error;
	if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
	USkeletalMeshComponent* Mesh = FindMesh(*World);
	if (!Mesh || !Mesh->GetPhysicsAsset()) { AddError(TEXT("missing support mesh")); return false; }
	TArray<float> Targets;
	for (const auto& A : RobotConfig.Actuators) { Targets.Add(A.DefaultPosition); }
	if (!Runtime.ApplyActuatorTargets(Targets, Error)) { AddError(Error); return false; }
	constexpr double Dt = 0.005;
	for (int32 Step = 0; Step < 800; ++Step) { if (!Tick(*this, *World, Dt)) { return false; } }
	double TotalMass = 0.0;
	TArray<FName> BodyNames;
	for (const USkeletalBodySetup* Setup : Mesh->GetPhysicsAsset()->SkeletalBodySetups)
	{
		if (!Setup) { AddError(TEXT("support inventory has a missing body")); return false; }
		FBodyInstance* Body = Mesh->GetBodyInstance(Setup->BoneName);
		if (!Body || !Body->IsValidBodyInstance()) { AddError(TEXT("support body is not simulated")); return false; }
		BodyNames.Add(Setup->BoneName);
		TotalMass += Body->GetBodyMass();
	}
	auto* ContactContainer = CompletedContactContainer(*this, *World);
	if (!ContactContainer) { return false; }
	const auto* SolverTypeOverride = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.Solver.Collision.SolverType"));
	AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] contact_solver_type=%d override=%d"),
		static_cast<int32>(ContactContainer->GetSolverType()), SolverTypeOverride ? SolverTypeOverride->GetInt() : -999));
	const FMomentum Before = Momentum(*Mesh);
	FVector Origin = FVector::ZeroVector;
	for (FName Name : BodyNames)
	{
		const auto* Body = Mesh->GetBodyInstance(Name);
		Origin += Body->GetCOMPosition() * (Body->GetBodyMass() / (100.0 * TotalMass));
	}
	const FVector Gravity(0.0, 0.0, World->GetGravityZ() / 100.0);
	FVector ContactAngularImpulse = FVector::ZeroVector;
	FVector GravityAngularImpulse = FVector::ZeroVector;
	double MaximumSpeed = 0.0;
	double MaximumAngularSpeed = 0.0;
	TMap<FName, FVector> BodyImpulse;
	FVector Sum = FVector::ZeroVector;
	double MinimumSupport = TNumericLimits<double>::Max();
	double MaximumSupport = 0.0;
	for (int32 Step = 0; Step < 400; ++Step)
	{
		// Gravity acts during the step at the pre-step COM. Integrate its
		// moment about a fixed origin, not a changing support centroid.
		for (FName Name : BodyNames)
		{
			const auto* Body = Mesh->GetBodyInstance(Name);
			GravityAngularImpulse += FVector::CrossProduct(Body->GetCOMPosition() / 100.0 - Origin,
				Gravity * (Body->GetBodyMass() * Dt));
		}
		if (!Tick(*this, *World, Dt)) { return false; }
		FVector StepImpulse = FVector::ZeroVector;
		TMap<FName, FVector> StepByBody;
		FPhysicsCommand::ExecuteRead(Mesh, [&]()
		{
			auto* Collisions = CompletedContactContainer(*this, *World);
			if (!Collisions) { return; }
			auto& Allocator = Collisions->GetConstraintAllocator();
			TSet<Chaos::FGeometryParticleHandle*> RobotParticles;
			for (FBodyInstance* Body : Mesh->Bodies)
			{
				if (Body && Body->GetPhysicsActorHandle()) { RobotParticles.Add(Body->GetPhysicsActorHandle()->GetHandle_LowLevel()); }
			}
			for (FName Name : BodyNames)
			{
				const auto Handle = Mesh->GetBodyInstance(Name)->GetPhysicsActorHandle();
				auto* Particle = Handle && Handle->GetHandle_LowLevel() ? Handle->GetHandle_LowLevel()->CastToRigidParticle() : nullptr;
				if (!Particle) { AddError(TEXT("missing support particle")); return; }
				FVector J = FVector::ZeroVector;
				for (const auto* C : Collisions->GetConstraints())
				{
					if (!C || (C->GetParticle0() != Particle && C->GetParticle1() != Particle)) { continue; }
					auto* Other = C->GetParticle0() == Particle ? C->GetParticle1() : C->GetParticle0();
					if (!Other || RobotParticles.Contains(Other)) { continue; }
					if (!AccumulateContactWrench(*this, *C, Allocator, Particle, Dt, Origin, J, ContactAngularImpulse)) { return; }
				}
				StepByBody.Add(Name, J);
				BodyImpulse.FindOrAdd(Name) += J;
				StepImpulse += J;
			}
		});
		// A D4 observation describes the final solver step, not an average
		// obtained by dividing that step's impulse by a four-step window.
		if (Step % 4 == 3)
		{
			Runtime.SamplePhysicsContacts(Dt);
			TArray<float> State;
			if (!Runtime.CollectState(State, Error) || State.Num() != 55)
			{
				AddError(TEXT("support capture requires all 19 force observations: ") + Error);
				return false;
			}
			for (int32 Index = 0; Index < ForceBodies.Num(); ++Index)
			{
				const double ExpectedForce = StepByBody.FindOrAdd(ForceBodies[Index]).Size() / Dt;
				TestTrue(TEXT("published final-step contact force equals that completed step's impulse over physics dt"),
					FMath::Abs(State[36 + Index] - ExpectedForce) <= 0.001 + ExpectedForce * 0.005);
			}
		}
		for (FName Name : BodyNames)
		{
			const auto* Body = Mesh->GetBodyInstance(Name);
			MaximumSpeed = FMath::Max(MaximumSpeed, Body->GetUnrealWorldVelocity().Size() / 100.0);
			MaximumAngularSpeed = FMath::Max(MaximumAngularSpeed, Body->GetUnrealWorldAngularVelocityInRadians().Size());
		}
		Sum += StepImpulse;
		MinimumSupport = FMath::Min(MinimumSupport, StepImpulse.Z / Dt);
		MaximumSupport = FMath::Max(MaximumSupport, StepImpulse.Z / Dt);
	}
	constexpr double Duration = 400 * Dt;
	const FVector Weight = -Gravity * TotalMass;
	const FVector MeanSupport = Sum / Duration;
	const double Budget = 0.05 + 0.03 * Weight.Size();
	TestTrue(TEXT("whole-robot signed mean contact force balances actual weight"), Matches(MeanSupport, Weight, Budget));
	TestTrue(TEXT("contact plus gravity impulse explains whole-body momentum change"),
		Matches(Momentum(*Mesh).Linear - Before.Linear, Sum + Gravity * (TotalMass * Duration), Budget * Duration));
	const FMomentum After = Momentum(*Mesh);
	const FVector AngularChange = After.Angular - Before.Angular
		- FVector::CrossProduct(Origin, After.Linear - Before.Linear);
	const FVector ExternalAngularImpulse = ContactAngularImpulse + GravityAngularImpulse;
	// 5 mN m absolute plus 3% of gravity moment, fixed before execution.
	const double MomentBudget = 0.005 + 0.03 * GravityAngularImpulse.Size() / Duration;
	TestTrue(TEXT("per-point contact and gravity moments explain whole-body angular momentum change"),
		Matches(AngularChange, ExternalAngularImpulse, MomentBudget * Duration));
	TestTrue(TEXT("settled support balances all three external moment components"),
		ExternalAngularImpulse.Size() / Duration <= MomentBudget);
	TestTrue(TEXT("weight-support fixture remains at rest throughout measurement"),
		MaximumSpeed < 0.01 && MaximumAngularSpeed < 0.1);
	AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] support_moment contact_nms=%s gravity_nms=%s delta_h=%s error_nms=%.9f budget_nm=%.9f max_v=%.9f max_w=%.9f"),
		*PreciseVector(ContactAngularImpulse), *PreciseVector(GravityAngularImpulse), *PreciseVector(AngularChange),
		(AngularChange - ExternalAngularImpulse).Size(), MomentBudget, MaximumSpeed, MaximumAngularSpeed));
	AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] support mass=%.9f weight=%s mean=%s min_z=%.9f max_z=%.9f budget_n=%.9f"),
		TotalMass, *Weight.ToString(), *MeanSupport.ToString(), MinimumSupport, MaximumSupport, Budget));
	for (FName Name : BodyNames)
	{
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] body=%s mean_external_contact_n=%s"),
			*Name.ToString(), *(BodyImpulse.FindOrAdd(Name) / Duration).ToString()));
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLGeometricSupportWithoutForceTest,
    "UERL.Integration.PhysicsResponse.PhantomX.GeometricSupportIsNotPhysicalForce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGeometricSupportWithoutForceTest::RunTest(const FString& Parameters)
{
    using namespace UERLPhysicsResponseTests;
    using namespace UERLPhantomXPhysicsTests;
    FLockstepSettings Settings;
    auto Scene = MakeScene();
    UWorld* World = Scene->GetWorld();
    if (!World || !Ground(*World)) { AddError(TEXT("missing geometric support fixture")); return false; }
    UStaticMeshComponent* Plane = nullptr;
    for (TActorIterator<AStaticMeshActor> It(World); It; ++It) { Plane = It->GetStaticMeshComponent(); break; }
    if (!Plane) { AddError(TEXT("missing support plane")); return false; }
    Plane->SetMobility(EComponentMobility::Movable);
    Plane->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Plane->SetCollisionObjectType(ECC_WorldStatic);
    Plane->SetCollisionResponseToAllChannels(ECR_Block);
    auto RobotConfig = Config(3.0);
    RobotConfig.Observations.Reset();
    RobotConfig.Observations.Add({ EUERLObservationType::Contact, FName(TEXT("base_link")) });
    RobotConfig.Observations.Add({ EUERLObservationType::ContactForce, FName(TEXT("base_link")) });
    FUERLSkeletalMeshRobotRuntime Runtime;
    FString Error;
    if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
    USkeletalMeshComponent* Mesh = FindMesh(*World);
    FBodyInstance* Base = Mesh ? Mesh->GetBodyInstance(FName(TEXT("base_link"))) : nullptr;
    if (!Mesh || !Base) { AddError(TEXT("missing physical PhantomX base")); return false; }
    for (FBodyInstance* Body : Mesh->Bodies) { if (Body) { Body->SetEnableGravity(false); } }
    // The product geometric probe spans 4 cm above and 3 cm below the body's
    // lowest bound. A query-only surface 1 cm below it is support geometry,
    // but cannot provide a solver impulse or a physical support force.
    const FVector ResetCOM = Base->GetCOMPosition();
    const double SurfaceZ = Base->GetBodyBounds().Min.Z - 1.0;
    Plane->SetWorldLocation(FVector(0.0, 0.0, SurfaceZ - 10.0));
    for (int32 Phase = 0; Phase < 2; ++Phase)
    {
        if (Phase == 1) { Plane->SetWorldLocation(FVector(0.0, 0.0, SurfaceZ - 110.0)); }
        if (!Tick(*this, *World, 0.005)) { return false; }
        Runtime.SamplePhysicsContacts(0.005);
        TestTrue(TEXT("the first solver steps preserve the explicit gravity-free reset placement"),
            (Base->GetCOMPosition() - ResetCOM).Size() < 1.0); // 1 cm allows internal settling, not a lost 3 m reset
        const FBox CurrentBounds = Base->GetBodyBounds();
        const FVector ProbeCenter(Base->GetUnrealWorldTransform().GetLocation().X,
            Base->GetUnrealWorldTransform().GetLocation().Y, CurrentBounds.Min.Z);
        FHitResult ProbeHit;
        FCollisionQueryParams ProbeParams(SCENE_QUERY_STAT(PhysicalSupportDiagnostic), false, Mesh->GetOwner());
        const bool ProbeFound = World->LineTraceSingleByObjectType(ProbeHit,
            ProbeCenter + FVector(0.0, 0.0, 4.0), ProbeCenter - FVector(0.0, 0.0, 3.0),
            FCollisionObjectQueryParams(ECC_WorldStatic), ProbeParams);
        AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] query_fixture phase=%d initial_surface_cm=%.9f bounds_min_cm=%s plane_cm=%s probe_hit=%d normal=%s"),
            Phase, SurfaceZ, *CurrentBounds.Min.ToString(), *Plane->GetComponentLocation().ToString(),
            ProbeFound ? 1 : 0, *ProbeHit.ImpactNormal.ToString()));
        TArray<float> State;
        if (!Runtime.CollectState(State, Error) || State.Num() != 2) { AddError(Error); return false; }
        TestTrue(TEXT("geometric support reflects the query surface independently of force"),
            FMath::Abs(State[0] - (Phase == 0 ? 1.0f : 0.0f)) < 1.0e-6);
        TestTrue(TEXT("query-only support cannot report physical contact force"), FMath::Abs(State[1]) < 1.0e-6);
        AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] query_support phase=%d geometric=%.6f force_n=%.9f"),
            Phase, State[0], State[1]));
    }
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhantomXBodyFeedbackTest,
    "UERL.Integration.PhysicsResponse.PhantomX.ArticulatedBodyCOMAndLinkFeedback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhantomXBodyFeedbackTest::RunTest(const FString& Parameters)
{
    using namespace UERLPhysicsResponseTests;
    using namespace UERLPhantomXPhysicsTests;
    FLockstepSettings Settings;
    auto Scene = MakeScene();
    UWorld* World = Scene->GetWorld();
    if (!World) { AddError(TEXT("missing articulated feedback World")); return false; }
    auto RobotConfig = Config(3.0);
    RobotConfig.Observations.Reset();
    // Exercise nonidentity field order, including articulated children.
    TArray<FName> Names;
    for (int32 J = RobotConfig.Actuators.Num() - 1; J >= 0; --J) { Names.Add(RobotConfig.Actuators[J].JointName); }
    for (FName Name : Names)
    {
        RobotConfig.Observations.Add({ EUERLObservationType::BodyPose, Name });
        RobotConfig.Observations.Add({ EUERLObservationType::BodyLinearVelocity, Name });
        RobotConfig.Observations.Add({ EUERLObservationType::BodyAngularVelocity, Name });
    }
    FUERLSkeletalMeshRobotRuntime Runtime;
    FString Error;
    if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
    auto* Mesh = FindMesh(*World);
    if (!Mesh) { AddError(TEXT("missing articulated feedback mesh")); return false; }
    Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
    const FVector Omega(0.2, 0.3, -0.4);
    const FVector Translation(0.3, -0.2, 0.1);
    const FVector Origin(0.0, 0.0, 3.0);
    // Test-local offset guarantees that using COM velocity as link-origin
    // velocity is observable, even if a future asset centers every body.
    Mesh->SetCenterOfMass(FVector(2.0, -1.0, 3.0), Names[0]);
    for (FBodyInstance* Body : Mesh->Bodies)
    {
        if (!Body) { continue; }
        Body->SetEnableGravity(false);
        Body->LinearDamping = 0.0f; Body->AngularDamping = 0.0f;
        Body->UpdateDampingProperties();
        const FVector V = Translation + FVector::CrossProduct(Omega, Body->GetCOMPosition() / 100.0 - Origin);
        Body->SetLinearVelocity(V * 100.0, false);
        Body->SetAngularVelocityInRadians(Omega, false);
    }
    int32 OffsetBodies = 0;
    for (int32 Step = 0; Step < 20; ++Step)
    {
        if (!Tick(*this, *World, 0.005)) { return false; }
        TArray<float> State;
        if (!Runtime.CollectState(State, Error) || State.Num() != Names.Num() * 13) { AddError(Error); return false; }
        for (int32 B = 0; B < Names.Num(); ++B)
        {
            FBodyInstance* Body = Mesh->GetBodyInstance(Names[B]);
            if (!Body) { AddError(TEXT("missing requested articulated body")); return false; }
            const FTransform Link = Body->GetUnrealWorldTransform();
            const FVector COM = Body->GetCOMPosition() / 100.0;
            const FVector V = Body->GetUnrealWorldVelocity() / 100.0;
            const FVector W = Body->GetUnrealWorldAngularVelocityInRadians();
            const FVector LinkOffset = Link.GetLocation() / 100.0 - COM;
            if (Step == 0 && LinkOffset.Size() > 1.0e-4) { ++OffsetBodies; }
            const FVector ExpectedLinkV = V + FVector::CrossProduct(W, LinkOffset);
            TestTrue(TEXT("articulated link velocity has the correct COM moment arm"),
                Matches(Body->GetUnrealWorldVelocityAtPoint(Link.GetLocation()) / 100.0, ExpectedLinkV, 1.0e-5));
            const int32 I = B * 13;
            const FVector Position(State[I], State[I + 1], State[I + 2]);
            const FQuat Rotation(State[I + 3], State[I + 4], State[I + 5], State[I + 6]);
            const FVector ReportedV(State[I + 7], State[I + 8], State[I + 9]);
            const FVector ReportedW(State[I + 10], State[I + 11], State[I + 12]);
            TestTrue(TEXT("named child pose feedback uses the declared SI link frame"),
                Matches(Position, Link.GetLocation() / 100.0, 1.0e-5)
                && Rotation.GetNormalized().AngularDistance(Link.GetRotation().GetNormalized()) < 1.0e-5);
            // The current body-linear-velocity field is COM velocity. Do not
            // compare it with the derivative of an offset link-origin pose.
            TestTrue(TEXT("named body velocity feedback preserves COM and angular semantics"),
                Matches(ReportedV, V, 1.0e-5) && Matches(ReportedW, W, 1.0e-5));
        }
    }
    TestTrue(TEXT("the real asset includes a nonzero COM/link offset in this test"), OffsetBodies > 0);
    AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] articulated_feedback bodies=%d offset_bodies=%d frames=20"), Names.Num(), OffsetBodies));
    return true;
}

#endif

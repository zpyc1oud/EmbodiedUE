#include "UERLPhysicsResponseTestSupport.h"

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
		Mesh->SetMobility(EComponentMobility::Static);
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
	for (const USkeletalBodySetup* Setup : Asset->SkeletalBodySetups)
	{
		if (!Setup) { AddError(TEXT("missing asset body setup")); return false; }
		TestFalse(TEXT("physical body names are unique"), Names.Contains(Setup->BoneName));
		Names.Add(Setup->BoneName);
		FBodyInstance* Body = Mesh->GetBodyInstance(Setup->BoneName);
		if (!Body || !Body->IsValidBodyInstance()) { AddError(TEXT("asset body has no live solver body")); return false; }
		const double Mass = Body->GetBodyMass();
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
		TestTrue(TEXT("each constraint refers to physical bodies"), Names.Contains(C->ConstraintBone1) && Names.Contains(C->ConstraintBone2));
		TestTrue(TEXT("each live joint has a declared actuator"), RobotConfig.Actuators.ContainsByPredicate(
			[C](const FUERLSkeletalMeshRuntimeActuator& A) { return A.JointName == C->JointName; }));
		const auto& Drive = C->ProfileInstance.AngularDrive;
		const auto& AxisDrive = C->GetAngularTwistMotion() != EAngularConstraintMotion::ACM_Locked
			? Drive.TwistDrive : Drive.SwingDrive;
		const double ActualKp = AxisDrive.Stiffness * Chaos::ConstraintSettings::AngularDriveStiffnessScale() / 10000.0;
		const double ActualKd = AxisDrive.Damping * Chaos::ConstraintSettings::AngularDriveDampingScale() / 10000.0;
		TestTrue(TEXT("declared SI stiffness reaches every PhantomX joint drive"), FMath::Abs(ActualKp - 25.0) < 1.0e-4);
		TestTrue(TEXT("declared SI damping reaches every PhantomX joint drive"), FMath::Abs(ActualKd - 0.5) < 1.0e-5);
		TestTrue(TEXT("every live drive uses the declared force-mode torque cap"),
			!Drive.bAccelerationMode && FMath::Abs(AxisDrive.MaxForce / 10000.0 - 2.8) < 1.0e-5);
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] joint=%s effective_kp_nm_rad=%.9f effective_kd_nm_s_rad=%.9f cap_nm=%.9f target=%s"),
			*C->JointName.ToString(), ActualKp, ActualKd, AxisDrive.MaxForce / 10000.0, *Drive.OrientationTarget.ToString()));
		FVector SolverTarget;
		FPhysicsInterface::GetDriveAngularVelocity(C->GetPhysicsConstraintRef(), SolverTarget);
		TestTrue(TEXT("zero requested drive speed reaches the solver"), SolverTarget.Size() < 1.0e-8);
		AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] joint=%s child=%s parent=%s twist_motion=%d twist_limit_deg=%.6f projection=%d mass_conditioning=%d frame1=%s frame2=%s"),
			*C->JointName.ToString(), *C->ConstraintBone1.ToString(), *C->ConstraintBone2.ToString(),
			static_cast<int32>(C->GetAngularTwistMotion()), C->GetAngularTwistLimit(), C->IsProjectionEnabled(),
			C->ProfileInstance.bEnableMassConditioning, *C->GetRefFrame(EConstraintFrame::Frame1).ToString(),
			*C->GetRefFrame(EConstraintFrame::Frame2).ToString()));
	}
	AddInfo(FString::Printf(TEXT("[PHYSICS_INVENTORY] total_mass_kg=%.9f; hardware mass provenance requires separate reviewed evidence"), TotalMass));
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
	for (int32 Decimation : { 1, 4 })
	{
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		if (!World || !Ground(*World)) { AddError(TEXT("missing state-consistency World")); return false; }
		const auto RobotConfig = Config();
		FUERLSkeletalMeshRobotRuntime Runtime;
		FString Error;
		if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
		TArray<float> Targets;
		for (const auto& A : RobotConfig.Actuators) { Targets.Add(A.DefaultPosition); }
		TArray<double> DeltaQ, IntegratedW, Variation;
		DeltaQ.Init(0.0, 18); IntegratedW.Init(0.0, 18); Variation.Init(0.0, 18);
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
				}
				Trace += FString::Printf(TEXT("%d,%lld,%.9f,%.9f,%d,%s,%.9f,%.9f,%.9f\n"), Step + 1,
					static_cast<long long>(Clock.Frame), Clock.SolverTime, Clock.LastDt, Decimation, *RobotConfig.Actuators[J].JointName.ToString(), Targets[J], Q, W);
			}
			Previous = State;
		}
		if (!SaveTrace(*this, FString::Printf(TEXT("phantomx-fixed-target-D%d"), Decimation), Trace)) { return false; }
		for (int32 J = 0; J < 18; ++J)
		{
			// Include a first-order variation budget and an absolute six-second
			// drift allowance. A persistent stationary q/nonzero qdot cannot pass.
			const double Tolerance = 0.008 + 0.5 * Dt * Variation[J];
			const FString Label = FString::Printf(TEXT("%s D%d position change agrees with solver-step velocity integral"),
				*RobotConfig.Actuators[J].JointName.ToString(), Decimation);
			TestTrue(*Label, FMath::Abs(DeltaQ[J] - IntegratedW[J]) <= Tolerance);
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] joint=%s D=%d delta_q=%.9f integral_qd=%.9f tolerance=%.9f"),
				*RobotConfig.Actuators[J].JointName.ToString(), Decimation, DeltaQ[J], IntegratedW[J], Tolerance));
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
	for (double Sign : { -1.0, 0.0, 1.0 })
	{
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		if (!World) { return false; }
		const auto RobotConfig = Config(3.0);
		FUERLSkeletalMeshRobotRuntime Runtime;
		FString Error;
		if (!Runtime.Initialize(*World, RobotConfig, Error)) { AddError(Error); return false; }
		USkeletalMeshComponent* Mesh = FindMesh(*World);
		if (!Mesh) { AddError(TEXT("momentum test requires the actual PhantomX")); return false; }
		Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
		for (FBodyInstance* Body : Mesh->Bodies)
		{
			if (!Body || !Body->IsValidBodyInstance()) { AddError(TEXT("missing momentum body")); return false; }
			Body->SetEnableGravity(false);
			Body->LinearDamping = 0.0f;
			Body->AngularDamping = 0.0f;
			Body->UpdateDampingProperties();
		}
		TArray<float> Targets;
		for (const auto& A : RobotConfig.Actuators) { Targets.Add(A.DefaultPosition); }
		if (!Runtime.ApplyActuatorTargets(Targets, Error) || !Tick(*this, *World, 0.005)) { AddError(Error); return false; }
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
		TestTrue(TEXT("internal articulation forces preserve total linear momentum"),
			Matches(After.Linear - Before.Linear, Impulse, 0.001 + Impulse.Size() * 0.02));
		TestTrue(TEXT("internal articulation torques preserve total angular momentum about the fixed world origin"),
			Matches(After.Angular - Before.Angular, ExpectedAngular, 0.0001 + ExpectedAngular.Size() * 0.02));
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] phantomx_momentum sign=%.0f expected_dp=%s actual_dp=%s expected_dL=%s actual_dL=%s"),
			Sign, *Impulse.ToString(), *(After.Linear - Before.Linear).ToString(),
			*ExpectedAngular.ToString(), *(After.Angular - Before.Angular).ToString()));
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
	const FMomentum Before = Momentum(*Mesh);
	TMap<FName, FVector> BodyImpulse;
	FVector Sum = FVector::ZeroVector;
	double MinimumSupport = TNumericLimits<double>::Max();
	double MaximumSupport = 0.0;
	for (int32 Step = 0; Step < 400; ++Step)
	{
		if (!Tick(*this, *World, Dt)) { return false; }
		FVector StepImpulse = FVector::ZeroVector;
		TMap<FName, FVector> StepByBody;
		FPhysicsCommand::ExecuteRead(Mesh, [&]()
		{
			TSet<Chaos::FGeometryParticleHandle*> RobotParticles;
			for (FBodyInstance* Body : Mesh->Bodies)
			{
				if (Body && Body->GetPhysicsActorHandle()) { RobotParticles.Add(Body->GetPhysicsActorHandle()->GetHandle_LowLevel()); }
			}
			for (FName Name : BodyNames)
			{
				const auto Handle = Mesh->GetBodyInstance(Name)->GetPhysicsActorHandle();
				auto* Particle = Handle && Handle->GetHandle_LowLevel() ? Handle->GetHandle_LowLevel()->CastToRigidParticle() : nullptr;
				if (!Particle) { continue; }
				FVector J = FVector::ZeroVector;
				Particle->ParticleCollisions().VisitConstCollisions([&](const Chaos::FPBDCollisionConstraint& C)
				{
					auto* Other = C.GetParticle0() == Particle ? C.GetParticle1() : C.GetParticle0();
					if (!Other || RobotParticles.Contains(Other)) { return Chaos::ECollisionVisitorResult::Continue; }
					const double Sign = C.GetParticle0() == Particle ? 1.0 : -1.0;
					J += Sign * FVector(C.AccumulatedImpulse.X, C.AccumulatedImpulse.Y, C.AccumulatedImpulse.Z) / 100.0;
					return Chaos::ECollisionVisitorResult::Continue;
				});
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
		Sum += StepImpulse;
		MinimumSupport = FMath::Min(MinimumSupport, StepImpulse.Z / Dt);
		MaximumSupport = FMath::Max(MaximumSupport, StepImpulse.Z / Dt);
	}
	constexpr double Duration = 400 * Dt;
	const FVector Gravity(0.0, 0.0, World->GetGravityZ() / 100.0);
	const FVector Weight = -Gravity * TotalMass;
	const FVector MeanSupport = Sum / Duration;
	const double Budget = 0.05 + 0.03 * Weight.Size();
	TestTrue(TEXT("whole-robot signed mean contact force balances actual weight"), Matches(MeanSupport, Weight, Budget));
	TestTrue(TEXT("contact plus gravity impulse explains whole-body momentum change"),
		Matches(Momentum(*Mesh).Linear - Before.Linear, Sum + Gravity * (TotalMass * Duration), Budget * Duration));
	AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] support mass=%.9f weight=%s mean=%s min_z=%.9f max_z=%.9f budget_n=%.9f"),
		TotalMass, *Weight.ToString(), *MeanSupport.ToString(), MinimumSupport, MaximumSupport, Budget));
	for (FName Name : BodyNames)
	{
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] body=%s mean_external_contact_n=%s"),
			*Name.ToString(), *(BodyImpulse.FindOrAdd(Name) / Duration).ToString()));
	}
	return true;
}

#endif

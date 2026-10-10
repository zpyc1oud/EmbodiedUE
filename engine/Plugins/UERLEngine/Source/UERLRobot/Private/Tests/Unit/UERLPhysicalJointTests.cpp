#include "UERLPhysicsResponseTestSupport.h"
#include "Chaos/ChaosConstraintSettings.h"
#include "Chaos/PBDJointConstraintData.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLJointCoordinateResponseTest,
	"UERL.Integration.PhysicsResponse.Joint.CoordinateSignAndLockedAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLJointCoordinateResponseTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	// Literal axis declarations are independent of GenericJointCoordinateAxis.
	const EUERLJointCoordinate Coordinates[] = { EUERLJointCoordinate::Twist,
		EUERLJointCoordinate::Swing1, EUERLJointCoordinate::Swing2,
		EUERLJointCoordinate::LinearX, EUERLJointCoordinate::LinearY, EUERLJointCoordinate::LinearZ };
	const FVector Axes[] = { FVector::XAxisVector, FVector::ZAxisVector, FVector::YAxisVector,
		FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector };
	for (int32 Index = 0; Index < 6; ++Index)
	{
		for (double Sign : { -1.0, 1.0 })
		{
			auto Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			UStaticMeshComponent* Cube = World ? MakeCube(*World, 10.0) : nullptr;
			if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing coordinate fixture")); return false; }
			UPhysicsConstraintComponent* Joint = PinCube(*Cube, 0.0, 0.0, 1.0);
			const FQuat Rotation = FRotator(23.0, -37.0, 19.0).Quaternion();
			Joint->SetWorldRotation(Rotation);
			Joint->SetAngularTwistLimit(Index == 0 ? EAngularConstraintMotion::ACM_Free : EAngularConstraintMotion::ACM_Locked, 0.0f);
			Joint->SetAngularSwing1Limit(Index == 1 ? EAngularConstraintMotion::ACM_Free : EAngularConstraintMotion::ACM_Locked, 0.0f);
			Joint->SetAngularSwing2Limit(Index == 2 ? EAngularConstraintMotion::ACM_Free : EAngularConstraintMotion::ACM_Locked, 0.0f);
			Joint->SetLinearXLimit(Index == 3 ? ELinearConstraintMotion::LCM_Free : ELinearConstraintMotion::LCM_Locked, 0.0f);
			Joint->SetLinearYLimit(Index == 4 ? ELinearConstraintMotion::LCM_Free : ELinearConstraintMotion::LCM_Locked, 0.0f);
			Joint->SetLinearZLimit(Index == 5 ? ELinearConstraintMotion::LCM_Free : ELinearConstraintMotion::LCM_Locked, 0.0f);
			// Recreate reference frames after changing the fixture orientation.
			Joint->SetConstrainedComponents(Cube, NAME_None, nullptr, NAME_None);
			Joint->ConstraintInstance.DisableProjection();
			Joint->ConstraintInstance.SetOrientationDriveTwistAndSwing(false, false);
			Joint->ConstraintInstance.SetAngularVelocityDriveTwistAndSwing(false, false);
			if (!Tick(*this, *World, 0.005)) { return false; }
			FBodyInstance* Body = Cube->GetBodyInstance();
			const FTransform Start = Body->GetUnrealWorldTransform();
			const FVector Axis = Rotation.RotateVector(Axes[Index]);
			const FVector CrossAxis = Rotation.RotateVector(Axes[Index].X == 1.0 ? FVector::YAxisVector : FVector::XAxisVector);
			const bool Angular = Index < 3;
			const double Input = Angular ? 0.01 : 1.0;
			for (int32 Step = 0; Step < 20; ++Step)
			{
				const FVector Applied = Axis * ConvertGenericJointEffortToChaos(Sign * Input, Coordinates[Index]);
				if (Angular)
				{
					Cube->AddTorqueInRadians(Applied + CrossAxis * 100.0, NAME_None, false);
					Cube->AddForce(CrossAxis * 100.0, NAME_None, false);
				}
				else
				{
					Cube->AddForce(Applied + CrossAxis * 100.0, NAME_None, false);
					Cube->AddTorqueInRadians(CrossAxis * 100.0, NAME_None, false);
				}
				if (!Tick(*this, *World, 0.005)) { return false; }
			}
			const FTransform End = Body->GetUnrealWorldTransform();
			const FVector Translation = (End.GetLocation() - Start.GetLocation()) / 100.0;
			const FVector W = Body->GetUnrealWorldAngularVelocityInRadians();
			const FVector V = Body->GetUnrealWorldVelocity() / 100.0;
			const double ExpectedSpeed = Sign * Input * 0.1 / (Angular ? 10.0 * 0.04 / 6.0 : 10.0);
			TestTrue(TEXT("the declared free coordinate has the predicted signed velocity"),
				Matches(Angular ? W : V, Axis * ExpectedSpeed, 1.0e-4 + FMath::Abs(ExpectedSpeed) * 0.005));
			const double GeometricQ = Angular
				? (End.GetRotation() * Start.GetRotation().Inverse()).GetNormalized().GetTwistAngle(Axis)
				: FVector::DotProduct(Translation, Axis);
			TestTrue(TEXT("independent geometry has the declared command sign"), Sign * GeometricQ > 0.0);
			TestTrue(TEXT("locked translations remain below half a millimetre"),
				(Angular ? Translation : Translation - Axis * GeometricQ).Size() < 0.0005);
			TestTrue(TEXT("locked angular coordinates remain still"),
				(Angular ? W - Axis * FVector::DotProduct(W, Axis) : W).Size() < 0.001);
			const FTransform Child = Joint->ConstraintInstance.GetRefFrame(EConstraintFrame::Frame1) * End;
			const double Reported = MeasureGenericJointPosition(Child,
				Joint->ConstraintInstance.GetRefFrame(EConstraintFrame::Frame2), Coordinates[Index]);
			TestTrue(TEXT("production joint readback agrees with independent geometric displacement"),
				FMath::Abs(Reported - GeometricQ) < 1.0e-4);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLJointTransientResponseTest,
	"UERL.Integration.PhysicsResponse.Joint.UnderdampedAndOverdampedStepResponse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLJointTransientResponseTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	constexpr double Inertia = 10.0 * 0.04 / 6.0;
	constexpr double Kp = 0.2;
	constexpr double Target = 0.1;
	for (double Kd : { 0.1, 0.3 })
	{
		for (double Dt : { 0.005, 0.0025 })
		{
			auto Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			UStaticMeshComponent* Cube = World ? MakeCube(*World, 10.0) : nullptr;
			if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing transient fixture")); return false; }
			UPhysicsConstraintComponent* Joint = PinCube(*Cube, Kp, Kd, 1.0);
			if (!Tick(*this, *World, Dt)) { return false; }
			Joint->ConstraintInstance.SetAngularOrientationTarget(FQuat(FVector::XAxisVector, Target));
			const double Alpha = Kd / (2.0 * Inertia);
			const double OmegaSquared = Kp / Inertia;
			const double Discriminant = Alpha * Alpha - OmegaSquared;
			double MaximumError = 0.0;
			double MaximumQ = 0.0;
			double LastOutsideSettlingBand = 0.0;
			// The fixed first-order bound uses the analytical decay rates, not a
			// tolerance fitted to a measured trajectory.
			const double PositionBudget = 1.0e-4 + 2.0 * Target * Dt * (Alpha + FMath::Sqrt(FMath::Abs(Discriminant)));
			const int32 Steps = FMath::RoundToInt(8.0 / Dt);
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				if (!Tick(*this, *World, Dt)) { return false; }
				const double T = (Step + 1) * Dt;
				double Expected = 0.0;
				if (Discriminant < 0.0)
				{
					const double Omega = FMath::Sqrt(-Discriminant);
					Expected = Target * (1.0 - FMath::Exp(-Alpha * T)
						* (FMath::Cos(Omega * T) + Alpha / Omega * FMath::Sin(Omega * T)));
				}
				else
				{
					const double Root = FMath::Sqrt(Discriminant);
					const double R1 = -Alpha + Root, R2 = -Alpha - Root;
					Expected = Target * (1.0 + (R2 * FMath::Exp(R1 * T) - R1 * FMath::Exp(R2 * T)) / (R1 - R2));
				}
				const double Q = JointPosition(*Cube, Joint->ConstraintInstance);
				if (!FMath::IsFinite(Q)) { AddError(TEXT("nonfinite transient state")); return false; }
				MaximumError = FMath::Max(MaximumError, FMath::Abs(Q - Expected));
				MaximumQ = FMath::Max(MaximumQ, Q);
				if (FMath::Abs(Q - Target) > Target * 0.02) { LastOutsideSettlingBand = T; }
			}
			TestTrue(TEXT("entire drive trajectory follows the independent second-order solution"), MaximumError <= PositionBudget);
			TestTrue(TEXT("drive settles within the measured eight-second window"), LastOutsideSettlingBand < 7.9);
			const double ExpectedOvershoot = Discriminant < 0.0 ? Target * FMath::Exp(-PI * Alpha / FMath::Sqrt(-Discriminant)) : 0.0;
			TestTrue(TEXT("overshoot agrees with the analytical damping regime"),
				FMath::Abs(FMath::Max(0.0, MaximumQ - Target) - ExpectedOvershoot) <= PositionBudget);
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] transient kd=%.4f dt=%.6f max_error=%.9f budget=%.9f overshoot=%.9f settling_s=%.6f"),
				Kd, Dt, MaximumError, PositionBudget, MaximumQ - Target, LastOutsideSettlingBand));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLJointLimitResponseTest,
	"UERL.Integration.PhysicsResponse.Joint.HardLimitsAndDisabledDrive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLJointLimitResponseTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double Sign : { -1.0, 1.0 })
	{
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		UStaticMeshComponent* Cube = World ? MakeCube(*World, 10.0) : nullptr;
		if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing limit fixture")); return false; }
		UPhysicsConstraintComponent* Joint = PinCube(*Cube, 2.0, 0.2, 0.2);
		Joint->ConstraintInstance.ProfileInstance.TwistLimit.bSoftConstraint = false;
		Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, FMath::RadiansToDegrees(0.2f));
		Joint->ConstraintInstance.SetAngularOrientationTarget(FQuat(FVector::XAxisVector, Sign * 0.5));
		for (int32 Step = 0; Step < 800; ++Step)
		{
			if (!Tick(*this, *World, 0.005)) { return false; }
			TestTrue(TEXT("hard angular limit does not permit more than 0.002 rad penetration"),
				FMath::Abs(JointPosition(*Cube, Joint->ConstraintInstance)) <= 0.202);
		}
		TestTrue(TEXT("out-of-range target reaches the intended signed stop"),
			FMath::Abs(JointPosition(*Cube, Joint->ConstraintInstance) - Sign * 0.2) <= 0.002);
		Joint->ConstraintInstance.SetAngularOrientationTarget(FQuat::Identity);
		for (int32 Step = 0; Step < 800; ++Step)
		{
			if (!Tick(*this, *World, 0.005)) { return false; }
			TestTrue(TEXT("inward recovery remains within the hard limit"),
				FMath::Abs(JointPosition(*Cube, Joint->ConstraintInstance)) <= 0.202);
		}
		TestTrue(TEXT("an inward target releases the stop and returns to zero"),
			FMath::Abs(JointPosition(*Cube, Joint->ConstraintInstance)) <= 0.002);
		Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Free, 0.0f);
		Joint->ConstraintInstance.SetOrientationDriveTwistAndSwing(false, false);
		Joint->ConstraintInstance.SetAngularVelocityDriveTwistAndSwing(false, false);
		Cube->GetBodyInstance()->SetAngularVelocityInRadians(FVector::XAxisVector * (Sign * 0.1), false);
		for (int32 Step = 0; Step < 40; ++Step)
		{
			if (!Tick(*this, *World, 0.005)) { return false; }
		}
		TestTrue(TEXT("disabled drive has no restoring or damping torque"),
			Matches(Cube->GetBodyInstance()->GetUnrealWorldAngularVelocityInRadians(), FVector::XAxisVector * (Sign * 0.1), 0.001));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPrismaticHardStopTest,
	"UERL.Integration.PhysicsResponse.Joint.PrismaticHardStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPrismaticHardStopTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double Sign : { -1.0, 1.0 })
	{
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		UStaticMeshComponent* Cube = World ? MakeCube(*World, 1.0) : nullptr;
		if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing prismatic stop body")); return false; }
		UPhysicsConstraintComponent* Joint = PinCube(*Cube, 0.0, 0.0, 1.0);
		Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0.0f);
		Joint->ConstraintInstance.ProfileInstance.LinearLimit.bSoftConstraint = false;
		Joint->SetLinearXLimit(ELinearConstraintMotion::LCM_Limited, 5.0f); // +/- 0.05 m
		if (!Tick(*this, *World, 0.005)) { return false; }
		const FVector Start = Cube->GetBodyInstance()->GetCOMPosition();
		for (int32 Step = 0; Step < 400; ++Step)
		{
			Cube->AddForce(FVector(Sign * 100.0, 0.0, 0.0), NAME_None, false);
			if (!Tick(*this, *World, 0.005)) { return false; }
			const FVector Change = (Cube->GetBodyInstance()->GetCOMPosition() - Start) / 100.0;
			TestTrue(TEXT("linear hard-stop penetration stays below 0.5 mm"), FMath::Abs(Change.X) <= 0.0505);
			TestTrue(TEXT("locked transverse coordinates remain fixed"), FMath::Abs(Change.Y) < 0.0005 && FMath::Abs(Change.Z) < 0.0005);
		}
		const double Q = (Cube->GetBodyInstance()->GetCOMPosition().X - Start.X) / 100.0;
		TestTrue(TEXT("signed force reaches the declared prismatic stop"), FMath::Abs(Q - Sign * 0.05) <= 0.0005);
		for (int32 Step = 0; Step < 400; ++Step)
		{
			Cube->AddForce(FVector(-Sign * 100.0, 0.0, 0.0), NAME_None, false);
			if (!Tick(*this, *World, 0.005)) { return false; }
			const double RecoveryQ = (Cube->GetBodyInstance()->GetCOMPosition().X - Start.X) / 100.0;
			TestTrue(TEXT("reversed force recovers within the prismatic limits"), FMath::Abs(RecoveryQ) <= 0.0505);
		}
		const double RecoveredQ = (Cube->GetBodyInstance()->GetCOMPosition().X - Start.X) / 100.0;
		TestTrue(TEXT("inward force releases the old stop and reaches the opposite stop"),
			FMath::Abs(RecoveredQ + Sign * 0.05) <= 0.0005);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLStaticAngularReactionTest,
	"UERL.Integration.PhysicsResponse.Joint.StaticAngularReactionAtCOM",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLStaticAngularReactionTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double Dt : { 0.005, 0.0025 })
	{
		for (double Sign : { -1.0, 1.0 })
		{
			auto Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			UStaticMeshComponent* Cube = World ? MakeCube(*World, 2.0) : nullptr;
			if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing reaction body")); return false; }
			UPhysicsConstraintComponent* Joint = PinCube(*Cube, 0.0, 0.0, 1.0);
			Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0.0f);
			Joint->ConstraintInstance.DisableMassConditioning();
			Joint->ConstraintInstance.SetOrientationDriveTwistAndSwing(false, false);
			Joint->ConstraintInstance.SetAngularVelocityDriveTwistAndSwing(false, false);
			Cube->SetEnableGravity(true);
			const FVector Force = Sign * FVector(1.0, 2.0, 3.0);
			const FVector Offset(0.1, -0.05, 0.02);
			const FVector Torque = Sign * FVector(0.03, 0.04, -0.02);
			// Reference point is the fixed world anchor at the body's COM.
			// Gravity has zero moment there. The declared offset force does not.
			const FVector Expected = -(Torque + FVector::CrossProduct(Offset, Force));
			FVector Mean = FVector::ZeroVector;
			const int32 Steps = FMath::RoundToInt(1.0 / Dt);
			for (int32 Step = 0; Step < Steps * 2; ++Step)
			{
				Cube->AddForceAtLocation(Force * 100.0, Cube->GetBodyInstance()->GetCOMPosition() + Offset * 100.0);
				Cube->AddTorqueInRadians(Torque * 10000.0, NAME_None, false);
				if (!Tick(*this, *World, Dt)) { return false; }
				if (Step >= Steps)
				{
					FVector LinearOutput, AngularImpulseOutput;
					Joint->ConstraintInstance.GetConstraintForce(LinearOutput, AngularImpulseOutput);
					// UE 5.8 JointConstraintProxy forwards GetAngularImpulse into
					// OutputData.Torque. Convert cm^2 to m^2 and divide by this dt.
					// This is total constraint reaction, not isolated motor torque.
					Mean += AngularImpulseOutput / (10000.0 * Dt * Steps);
				}
			}
			TestTrue(TEXT("static angular reaction balances applied torque and r cross F"),
				Matches(Mean, Expected, 0.002 + Expected.Size() * 0.02));
			TestTrue(TEXT("reaction fixture has settled rather than balancing acceleration"),
				Cube->GetBodyInstance()->GetUnrealWorldAngularVelocityInRadians().Size() < 0.005
				&& Cube->GetBodyInstance()->GetUnrealWorldVelocity().Size() / 100.0 < 0.002);
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] static_angular_reaction dt=%.6f expected_nm=%s measured_nm=%s"),
				Dt, *Expected.ToString(), *Mean.ToString()));
		}
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLHingeFiniteDifferenceTest,
    "UERL.Integration.PhysicsResponse.Joint.CompletedPoseVelocityAtTwoSteps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLHingeFiniteDifferenceTest::RunTest(const FString& Parameters)
{
    using namespace UERLPhysicsResponseTests;
    FLockstepSettings Settings;
    for (double Mass : { 1.0, 2.0 })
    for (double Dt : { 0.005, 0.0025 })
    for (double Sign : { -1.0, 1.0 })
    {
        auto Scene = MakeScene();
        UWorld* World = Scene->GetWorld();
        UStaticMeshComponent* Cube = World ? MakeCube(*World, Mass) : nullptr;
        if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing finite-difference fixture")); return false; }
        UPhysicsConstraintComponent* Joint = PinCube(*Cube, 0.0, 0.0, 1.0);
        Joint->ConstraintInstance.DisableMassConditioning();
        Joint->ConstraintInstance.SetOrientationDriveTwistAndSwing(false, false);
        Joint->ConstraintInstance.SetAngularVelocityDriveTwistAndSwing(false, false);
        if (!Tick(*this, *World, Dt)) { return false; }
        constexpr double Torque = 0.01;
        const double I = Mass * 0.04 / 6.0;
        const double Acceleration = Sign * Torque / I;
        TestTrue(TEXT("declared analytic inertia reaches the hinge body"),
            Matches(Cube->GetBodyInstance()->GetBodyInertiaTensor() / 10000.0, FVector(I), 1.0e-5));
        const FQuat Start = Cube->GetBodyInstance()->GetUnrealWorldTransform().GetRotation();
        TArray<double> Q, W;
        Q.Add(0.0); W.Add(Cube->GetBodyInstance()->GetUnrealWorldAngularVelocityInRadians().X);
        const int32 Steps = FMath::RoundToInt(0.2 / Dt);
        for (int32 Step = 0; Step < Steps; ++Step)
        {
            Cube->AddTorqueInRadians(FVector(Sign * Torque * 10000.0, 0.0, 0.0), NAME_None, false);
            if (!Tick(*this, *World, Dt)) { return false; }
            const FQuat Relative = (Cube->GetBodyInstance()->GetUnrealWorldTransform().GetRotation() * Start.Inverse()).GetNormalized();
            const double Angle = 2.0 * FMath::Atan2(Relative.X, Relative.W);
            Q.Add(Angle); W.Add(Cube->GetBodyInstance()->GetUnrealWorldAngularVelocityInRadians().X);
            const double T = (Step + 1) * Dt;
            TestTrue(TEXT("completed hinge velocity follows declared torque and inertia"),
                FMath::Abs(W.Last() - Acceleration * T) <= 0.0001 + 0.005 * FMath::Abs(Acceleration * T));
            TestTrue(TEXT("completed hinge angle follows declared acceleration"),
                FMath::Abs(Angle - 0.5 * Acceleration * T * T) <= 0.0001 + FMath::Abs(Acceleration) * T * Dt);
            TestTrue(TEXT("published coordinate matches independent body rotation"),
                FMath::Abs(JointPosition(*Cube, Joint->ConstraintInstance) - Angle) < 0.0001);
        }
        double MaximumFiniteDifferenceError = 0.0;
        for (int32 Step = 1; Step < Q.Num() - 1; ++Step)
        {
            const double Centered = (Q[Step + 1] - Q[Step - 1]) / (2.0 * Dt);
            MaximumFiniteDifferenceError = FMath::Max(MaximumFiniteDifferenceError, FMath::Abs(Centered - W[Step]));
        }
        // Constant acceleration has no centered-difference truncation term.
        // First-order solver position/velocity staggering contributes O(a*dt).
        const double Budget = 1.0e-6 / Dt + FMath::Abs(Acceleration) * Dt;
        TestTrue(TEXT("centered pose derivative matches velocity at the same completed frame"),
            MaximumFiniteDifferenceError <= Budget);
        AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] hinge_finite_difference mass=%.3f dt=%.6f max_error_rad_s=%.9f budget_rad_s=%.9f"),
            Mass, Dt, MaximumFiniteDifferenceError, Budget));
    }
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLStaticOffsetReactionWrenchTest,
    "UERL.Integration.PhysicsResponse.Joint.StaticOffsetReactionWrench",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLStaticOffsetReactionWrenchTest::RunTest(const FString& Parameters)
{
    using namespace UERLPhysicsResponseTests;
    FLockstepSettings Settings;
    for (double Mass : { 1.0, 2.0 })
    for (double Dt : { 0.005, 0.0025 })
    for (double Yaw : { 0.0, 37.0 })
    for (double Sign : { -1.0, 1.0 })
    {
        auto Scene = MakeScene();
        UWorld* World = Scene->GetWorld();
        auto* Cube = World ? MakeCube(*World, Mass) : nullptr;
        if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing offset-wrench fixture")); return false; }
        FBodyInstance* Body = Cube->GetBodyInstance();
        const FQuat Frame = FRotator(21.0, Yaw, -13.0).Quaternion();
        const FVector COM = Body->GetCOMPosition() / 100.0;
        const FVector AnchorToCOM = Frame.RotateVector(FVector(0.08, -0.04, 0.03));
        const FVector Anchor = COM - AnchorToCOM;
        auto* Joint = PinCube(*Cube, 0.0, 0.0, 1.0);
        Joint->SetWorldLocationAndRotation(Anchor * 100.0, Frame);
        Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0.0f);
        Joint->SetConstrainedComponents(Cube, NAME_None, nullptr, NAME_None);
        Joint->ConstraintInstance.DisableProjection();
        Joint->ConstraintInstance.DisableMassConditioning();
        Joint->ConstraintInstance.SetOrientationDriveTwistAndSwing(false, false);
        Joint->ConstraintInstance.SetAngularVelocityDriveTwistAndSwing(false, false);
        Cube->SetEnableGravity(true);
        const FVector Gravity(0.0, 0.0, World->GetGravityZ() / 100.0);
        const FVector Force = Frame.RotateVector(Sign * FVector(1.0, -2.0, 0.5));
        const FVector Torque = Frame.RotateVector(Sign * FVector(0.03, 0.04, -0.02));
        const FVector COMToApplication = Frame.RotateVector(FVector(-0.02, 0.05, 0.04));
        const FVector ExpectedForce = -(Mass * Gravity + Force);
        // The reported angular constraint impulse is a couple at the anchor.
        // Its separate linear impulse also creates a moment about the COM.
        const FVector ExpectedCouple = -(Torque
            + FVector::CrossProduct(AnchorToCOM + COMToApplication, Force)
            + FVector::CrossProduct(AnchorToCOM, Mass * Gravity));
        FVector MeanForce = FVector::ZeroVector, MeanCouple = FVector::ZeroVector;
        double MaximumSpeed = 0.0, MaximumAngularSpeed = 0.0;
        const int32 Samples = FMath::RoundToInt(1.0 / Dt);
        for (int32 Step = 0; Step < Samples * 2; ++Step)
        {
            Cube->AddForceAtLocation(Force * 100.0, (COM + COMToApplication) * 100.0);
            Cube->AddTorqueInRadians(Torque * 10000.0, NAME_None, false);
            if (!Tick(*this, *World, Dt)) { return false; }
            if (Step < Samples) { continue; }
            FVector LinearImpulse, AngularCoupleImpulse;
            Joint->ConstraintInstance.GetConstraintForce(LinearImpulse, AngularCoupleImpulse);
            // UE 5.8.3 PBDJointContainerSolver:273-274 and JointConstraintProxy:
            // the outputs are world-frame impulses on original Body1, despite
            // the Force/Torque API names. Body1 is the cube in this fixture.
            MeanForce += LinearImpulse / (100.0 * Dt * Samples);
            MeanCouple += AngularCoupleImpulse / (10000.0 * Dt * Samples);
            MaximumSpeed = FMath::Max(MaximumSpeed, Body->GetUnrealWorldVelocity().Size() / 100.0);
            MaximumAngularSpeed = FMath::Max(MaximumAngularSpeed, Body->GetUnrealWorldAngularVelocityInRadians().Size());
        }
        const double ForceBudget = 0.02 + 0.02 * ExpectedForce.Size();
        const double TorqueBudget = 0.002 + 0.02 * ExpectedCouple.Size();
        TestTrue(TEXT("all three reaction force components balance gravity and external force"),
            Matches(MeanForce, ExpectedForce, ForceBudget));
        TestTrue(TEXT("all three reaction couple components balance moments about the offset anchor"),
            Matches(MeanCouple, ExpectedCouple, TorqueBudget));
        const FVector ReactionAboutCOM = MeanCouple + FVector::CrossProduct(-AnchorToCOM, MeanForce);
        const FVector ExternalAboutCOM = Torque + FVector::CrossProduct(COMToApplication, Force);
        TestTrue(TEXT("couple plus the linear reaction moment balances the COM wrench"),
            (ReactionAboutCOM + ExternalAboutCOM).Size() <= TorqueBudget + AnchorToCOM.Size() * ForceBudget);
        TestTrue(TEXT("static wrench measurements exclude accelerating or drifting fixtures"),
            MaximumSpeed < 0.002 && MaximumAngularSpeed < 0.005
            && (Body->GetCOMPosition() / 100.0 - COM).Size() < 0.0005);
        TestFalse(TEXT("the same reaction oracle rejects a reversed body sign"),
            Matches(-MeanForce, ExpectedForce, ForceBudget));
        TestFalse(TEXT("the same reaction oracle rejects treating impulse as force"),
            Matches(MeanForce * Dt, ExpectedForce, ForceBudget));
        TestFalse(TEXT("the same reaction oracle rejects an unrequested local-frame rotation"),
            Matches(Frame.UnrotateVector(MeanForce), ExpectedForce, ForceBudget));
        TestFalse(TEXT("the moment balance detects omission of gravity and force moment arms"),
            Matches(MeanCouple, -Torque, TorqueBudget));
        AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] offset_wrench mass=%.6f dt=%.6f yaw=%.6f sign=%.0f expected_n=%s measured_n=%s error_n=%.9f budget_n=%.9f expected_nm=%s measured_nm=%s error_nm=%.9f budget_nm=%.9f"),
            Mass, Dt, Yaw, Sign, *ExpectedForce.ToString(), *MeanForce.ToString(),
            (MeanForce - ExpectedForce).Size(), ForceBudget, *ExpectedCouple.ToString(), *MeanCouple.ToString(),
            (MeanCouple - ExpectedCouple).Size(), TorqueBudget));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLSoftLimitResponseTest,
    "UERL.Integration.PhysicsResponse.Joint.SoftLimitForceModeResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSoftLimitResponseTest::RunTest(const FString& Parameters)
{
    using namespace UERLPhysicsResponseTests;
    FLockstepSettings Settings;
    for (bool Angular : { false, true })
    for (double Mass : { 1.0, 2.0 })
    for (double Dt : { 0.005, 0.0025 })
    for (double Sign : { -1.0, 1.0 })
    {
        auto Scene = MakeScene();
        UWorld* World = Scene->GetWorld();
        auto* Cube = World ? MakeCube(*World, Mass) : nullptr;
        if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing soft-limit body")); return false; }
        auto* Body = Cube->GetBodyInstance();
        auto* Joint = PinCube(*Cube, 0.0, 0.0, 1.0);
        auto& C = Joint->ConstraintInstance;
        C.DisableProjection();
        C.DisableMassConditioning();
        C.SetOrientationDriveTwistAndSwing(false, false);
        C.SetAngularVelocityDriveTwistAndSwing(false, false);
        const double EffectiveMass = Angular ? Mass * 0.04 / 6.0 : Mass;
        const double K = Angular ? 0.2 : 20.0;
        const double D = 3.0 * FMath::Sqrt(K * EffectiveMass); // overdamped
        const double Load = K * 0.01;
        const double Limit = Angular ? 0.1 : 0.05;
        const double KScale = Angular ? Chaos::ConstraintSettings::SoftAngularStiffnessScale()
            : Chaos::ConstraintSettings::SoftLinearStiffnessScale();
        const double DScale = Angular ? Chaos::ConstraintSettings::SoftAngularDampingScale()
            : Chaos::ConstraintSettings::SoftLinearDampingScale();
        if (!FMath::IsFinite(KScale) || !FMath::IsFinite(DScale) || KScale <= 0.0 || DScale <= 0.0)
        { AddError(TEXT("soft-limit coefficient scales must be positive and finite")); return false; }
        const double Units = Angular ? 10000.0 : 1.0;
        if (Angular)
        {
            Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, FMath::RadiansToDegrees(Limit));
            C.SetSoftTwistLimitParams(true, K * Units / KScale, D * Units / DScale, 0.0f, 0.0f);
        }
        else
        {
            Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Locked, 0.0f);
            Joint->SetLinearXLimit(ELinearConstraintMotion::LCM_Limited, Limit * 100.0);
            C.SetSoftLinearLimitParams(true, K / KScale, D / DScale, 0.0f, 0.0f);
        }
        bool ForceModeSet = false;
        FPhysicsCommand::ExecuteWrite(C.GetPhysicsConstraintRef(), [&](const FPhysicsConstraintHandle& Ref)
        {
            if (!Ref.Constraint || !Ref.Constraint->IsType(Chaos::EConstraintType::JointConstraintType)) { return; }
            auto* Native = static_cast<Chaos::FJointConstraint*>(Ref.Constraint);
            Native->SetLinearSoftForceMode(Chaos::EJointForceMode::Force);
            Native->SetAngularSoftForceMode(Chaos::EJointForceMode::Force);
            ForceModeSet = true;
        });
        if (!TestTrue(TEXT("soft-limit fixture explicitly selects force mode"), ForceModeSet)) { return false; }
        const FTransform Rest = Body->GetUnrealWorldTransform();
        FTransform Initial = Rest;
        if (Angular) { Initial.SetRotation(FQuat(FVector::XAxisVector, Sign * Limit) * Rest.GetRotation()); }
        else { Initial.AddToTranslation(FVector(Sign * Limit * 100.0, 0.0, 0.0)); }
        Body->SetBodyTransform(Initial, ETeleportType::TeleportPhysics);
        Body->SetLinearVelocity(FVector::ZeroVector, false);
        Body->SetAngularVelocityInRadians(FVector::ZeroVector, false);
        const double Alpha = D / (2.0 * EffectiveMass);
        const double Root = FMath::Sqrt(Alpha * Alpha - K / EffectiveMass);
        const double R1 = -Alpha + Root, R2 = -Alpha - Root;
        const double Budget = 1.0e-4 + 2.0 * (Load / K) * Dt * (Alpha + Root);
        double MaximumError = 0.0;
        auto ReadQ = [&]()
        {
            const auto Transform = Body->GetUnrealWorldTransform();
            if (!Angular) { return (Transform.GetLocation().X - Rest.GetLocation().X) / 100.0; }
            const auto Relative = (Transform.GetRotation() * Rest.GetRotation().Inverse()).GetNormalized();
            return 2.0 * FMath::Atan2(Relative.X, Relative.W);
        };
        for (int32 Step = 0; Step < FMath::RoundToInt(8.0 / Dt); ++Step)
        {
            if (Angular) { Cube->AddTorqueInRadians(FVector(Sign * Load * 10000.0, 0.0, 0.0), NAME_None, false); }
            else { Cube->AddForce(FVector(Sign * Load * 100.0, 0.0, 0.0), NAME_None, false); }
            if (!Tick(*this, *World, Dt)) { return false; }
            const double T = (Step + 1) * Dt;
            const double Expected = Sign * (Limit + Load / K
                * (1.0 + (R2 * FMath::Exp(R1 * T) - R1 * FMath::Exp(R2 * T)) / (R1 - R2)));
            const double Q = ReadQ();
            if (!FMath::IsFinite(Q)) { AddError(TEXT("nonfinite soft-limit trajectory")); return false; }
            MaximumError = FMath::Max(MaximumError, FMath::Abs(Q - Expected));
        }
        TestTrue(TEXT("soft-limit trajectory follows independent mass-spring-damper solution"), MaximumError <= Budget);
        TestTrue(TEXT("steady soft-limit penetration equals declared load over stiffness"),
            FMath::Abs(ReadQ() - Sign * (Limit + Load / K)) <= Budget);
        bool Released = false;
        for (int32 Step = 0; Step < FMath::RoundToInt(1.0 / Dt); ++Step)
        {
            if (Angular) { Cube->AddTorqueInRadians(FVector(-Sign * Load * 10000.0, 0.0, 0.0), NAME_None, false); }
            else { Cube->AddForce(FVector(-Sign * Load * 100.0, 0.0, 0.0), NAME_None, false); }
            if (!Tick(*this, *World, Dt)) { return false; }
            if (Sign * ReadQ() < Limit - 0.001) { Released = true; break; }
        }
        TestTrue(TEXT("inward load releases the penetrated soft stop"), Released);
        AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] soft_limit angular=%d mass=%.6f dt=%.6f sign=%.0f k=%.9f d=%.9f max_error=%.9f budget=%.9f released=%d"),
            Angular, Mass, Dt, Sign, K, D, MaximumError, Budget, Released));
    }
    return true;
}

#endif

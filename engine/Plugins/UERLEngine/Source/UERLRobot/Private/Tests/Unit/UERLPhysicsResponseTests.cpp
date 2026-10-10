#include "UERLPhysicsResponseTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UERLPhysicsResponseTests
{
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLAngularDampingResponseTest,
	"UERL.Integration.PhysicsResponse.Joint.AngularDampingDecay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLAngularDampingResponseTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double Dt : { 0.005, 0.0025 })
	{
		for (double Sign : { -1.0, 1.0 })
		{
			TUniquePtr<FPreviewScene> Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			UStaticMeshComponent* Cube = World ? MakeCube(*World, 10.0) : nullptr;
			if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing damping body")); return false; }
			UPhysicsConstraintComponent* Joint = PinCube(*Cube, 0.0, 0.2, 0.2);
			if (!Tick(*this, *World, Dt)) { return false; }
			FBodyInstance* Body = Cube->GetBodyInstance();
			Body->SetAngularVelocityInRadians(FVector::XAxisVector * (Sign * 0.4), false);
			constexpr double Duration = 0.2;
			const int32 Steps = FMath::RoundToInt(Duration / Dt);
			double PreviousSpeed = 0.4;
			for (int32 Step = 0; Step < Steps; ++Step)
			{
				if (!Tick(*this, *World, Dt)) { return false; }
				const double Speed = Body->GetUnrealWorldAngularVelocityInRadians().Size();
				TestTrue(TEXT("an unforced damping drive cannot increase kinetic energy"), Speed <= PreviousSpeed + 1.0e-6);
				PreviousSpeed = Speed;
			}
			// I*w_dot = -Kd*w. Predict from the declared SI parameters.
			constexpr double InertiaSi = 10.0 * 0.2 * 0.2 / 6.0;
			constexpr double Rate = 0.2 / InertiaSi;
			const double ExpectedW = Sign * 0.4 * FMath::Exp(-Rate * Duration);
			// Implicit Euler has log(1+x) >= x-x^2/2 for x >= 0.
			// This bounds its endpoint error above the continuous exponential.
			const double Tolerance = 1.0e-4 + FMath::Abs(ExpectedW)
				* (FMath::Exp(Rate * Rate * Duration * Dt / 2.0) - 1.0);
			const FVector ActualW = Body->GetUnrealWorldAngularVelocityInRadians();
			TestTrue(TEXT("angular damping follows the declared physical decay rate"),
				Matches(ActualW, FVector::XAxisVector * ExpectedW, Tolerance));
			ReportHinge(*this, *Cube, Joint->ConstraintInstance, TEXT("damping"), Steps);
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] damping dt=%.6f expected_w=%.9f actual_w=%.9f tolerance=%.9f"),
				Dt, ExpectedW, ActualW.X, Tolerance));
		}
	}
	return true;
}

#endif

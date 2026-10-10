#include "UERLPhysicsResponseTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhysicalInputLifetimeTest,
	"UERL.Integration.PhysicsResponse.FreeBody.ImpulseAndForceLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhysicalInputLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double Mass : { 1.0, 2.0 })
	{
		for (double Sign : { -1.0, 1.0 })
		{
			auto Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			UStaticMeshComponent* Cube = World ? MakeCube(*World, Mass) : nullptr;
			if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing impulse body")); return false; }
			constexpr double Dt = 0.005;
			if (!Tick(*this, *World, Dt)) { return false; }
			// One impulse is not repeated at every substep of a held control window.
			Cube->AddImpulse(FVector(Sign * 20.0, 0.0, 0.0), NAME_None, false); // 0.2 N s
			for (int32 Step = 0; Step < 4; ++Step)
			{
				if (!Tick(*this, *World, Dt)) { return false; }
				TestTrue(TEXT("a single impulse changes velocity once"), Matches(
					Cube->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0,
					FVector(Sign * 0.2 / Mass, 0.0, 0.0), 1.0e-5));
			}
			// A 1 N force held for four 5 ms steps adds 0.02 N s.
			for (int32 Step = 0; Step < 4; ++Step)
			{
				Cube->AddForce(FVector(Sign * 100.0, 0.0, 0.0), NAME_None, false);
				if (!Tick(*this, *World, Dt)) { return false; }
			}
			const FVector Expected(Sign * 0.22 / Mass, 0.0, 0.0);
			TestTrue(TEXT("held force contributes every completed step"), Matches(
				Cube->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0, Expected, 1.0e-5));
			for (int32 Step = 0; Step < 4; ++Step)
			{
				if (!Tick(*this, *World, Dt)) { return false; }
			}
			TestTrue(TEXT("zero command clears force rather than reusing the last force"), Matches(
				Cube->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0, Expected, 1.0e-5));
			Cube->AddImpulse(FVector(-Sign * 22.0, 0.0, 0.0), NAME_None, false);
			if (!Tick(*this, *World, Dt)) { return false; }
			TestTrue(TEXT("equal opposite accumulated impulse stops the body"), Matches(
				Cube->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0, FVector::ZeroVector, 1.0e-5));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLOffsetForceFrameTest,
	"UERL.Integration.PhysicsResponse.FreeBody.OffsetForceWorldAndLocalFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLOffsetForceFrameTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (const FQuat& Rotation : { FQuat::Identity, FRotator(25.0, -47.0, 18.0).Quaternion() })
	{
		for (bool Local : { false, true })
		{
			for (double Offset : { 0.0, 0.1 })
			{
				auto Scene = MakeScene();
				UWorld* World = Scene->GetWorld();
				UStaticMeshComponent* Cube = World ? MakeCube(*World, 1.0) : nullptr;
				if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing offset-force body")); return false; }
				Cube->SetWorldRotation(Rotation, false, nullptr, ETeleportType::TeleportPhysics);
				constexpr double Dt = 0.005;
				if (!Tick(*this, *World, Dt)) { return false; }
				FBodyInstance* Body = Cube->GetBodyInstance();
				const FVector ForceLocal(0.0, 0.5, 0.0); // N
				const FVector OffsetLocal(Offset, 0.0, 0.0); // m
				const FVector ForceWorld = Rotation.RotateVector(ForceLocal);
				const FVector OffsetWorld = Rotation.RotateVector(OffsetLocal);
				if (Local)
				{
					Cube->AddForceAtLocationLocal(ForceLocal * 100.0, OffsetLocal * 100.0);
				}
				else
				{
					Cube->AddForceAtLocation(ForceWorld * 100.0, Body->GetCOMPosition() + OffsetWorld * 100.0);
				}
				if (!Tick(*this, *World, Dt)) { return false; }
				const FVector ExpectedV = ForceWorld * Dt;
				const FVector ExpectedW = FVector::CrossProduct(OffsetWorld, ForceWorld) * (Dt / (0.04 / 6.0));
				TestTrue(TEXT("offset force preserves the declared linear impulse"), Matches(
					Body->GetUnrealWorldVelocity() / 100.0, ExpectedV, 1.0e-5 + ExpectedV.Size() * 0.005));
				TestTrue(TEXT("offset force produces r cross F angular impulse in the correct frame"), Matches(
					Body->GetUnrealWorldAngularVelocityInRadians(), ExpectedW, 1.0e-5 + ExpectedW.Size() * 0.005));
				AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] offset_force local=%d offset_m=%.3f expected_w=%s actual_w=%s"),
					Local, Offset, *ExpectedW.ToString(), *Body->GetUnrealWorldAngularVelocityInRadians().ToString()));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLAsymmetricInertiaTest,
	"UERL.Integration.PhysicsResponse.FreeBody.AsymmetricPrincipalInertia",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLAsymmetricInertiaTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	const FVector Dimensions(0.2, 0.3, 0.4);
	// A 2 kg uniform box. Expected principal inertias are from its declared dimensions.
	const FVector Inertia(2.0 * (0.09 + 0.16) / 12.0,
		2.0 * (0.04 + 0.16) / 12.0, 2.0 * (0.04 + 0.09) / 12.0);
	for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
	{
		for (double Sign : { -1.0, 1.0 })
		{
			auto Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			UStaticMeshComponent* Box = World ? MakeCube(*World, 2.0, Dimensions) : nullptr;
			if (!Box || !Box->GetBodyInstance()) { AddError(TEXT("missing asymmetric box")); return false; }
			constexpr double Dt = 0.005;
			if (!Tick(*this, *World, Dt)) { return false; }
			FVector LocalAxis = FVector::ZeroVector;
			LocalAxis[AxisIndex] = Sign;
			const FVector WorldAxis = FRotator(17.0, 31.0, -13.0).Quaternion().RotateVector(LocalAxis);
			for (int32 Step = 0; Step < 20; ++Step)
			{
				Box->AddTorqueInRadians(WorldAxis * 100.0, NAME_None, false); // 0.01 N m
				if (!Tick(*this, *World, Dt)) { return false; }
			}
			const FVector Expected = WorldAxis * (0.01 * 0.1 / Inertia[AxisIndex]);
			const FVector Actual = Box->GetBodyInstance()->GetUnrealWorldAngularVelocityInRadians();
			TestTrue(TEXT("principal-axis torque uses the correct nonuniform inertia"),
				Matches(Actual, Expected, 1.0e-4 + Expected.Size() * 0.005));
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] asymmetric axis=%d sign=%.0f expected=%s actual=%s"),
				AxisIndex, Sign, *Expected.ToString(), *Actual.ToString()));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLGravityCompensationTest,
	"UERL.Integration.PhysicsResponse.FreeBody.GravityAndSelectedCompensation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGravityCompensationTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	auto Scene = MakeScene();
	UWorld* World = Scene->GetWorld();
	if (!World) { AddError(TEXT("missing gravity World")); return false; }
	UStaticMeshComponent* Held = MakeCube(*World, 2.0);
	UStaticMeshComponent* Falling = MakeCube(*World, 1.0);
	if (!Held || !Falling) { AddError(TEXT("missing gravity bodies")); return false; }
	constexpr double Dt = 0.005;
	if (!Tick(*this, *World, Dt)) { return false; }
	const FVector HeldStart = Held->GetBodyInstance()->GetCOMPosition() / 100.0;
	const FVector FallStart = Falling->GetBodyInstance()->GetCOMPosition() / 100.0;
	const FVector Gravity(0.0, 0.0, World->GetGravityZ() / 100.0);
	TestTrue(TEXT("the gravity case has nonzero declared gravity"), Gravity.Z < -1.0);
	Held->SetEnableGravity(true);
	Falling->SetEnableGravity(true);
	for (int32 Step = 0; Step < 40; ++Step)
	{
		Held->AddForce(-Gravity * (2.0 * 100.0), NAME_None, false);
		if (!Tick(*this, *World, Dt)) { return false; }
	}
	TestTrue(TEXT("only the selected body receives gravity compensation"), Matches(
		Held->GetBodyInstance()->GetCOMPosition() / 100.0, HeldStart, 1.0e-4));
	TestTrue(TEXT("gravity compensated velocity remains zero"), Matches(
		Held->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0, FVector::ZeroVector, 1.0e-4));
	TestTrue(TEXT("uncompensated body has the analytical falling velocity"), Matches(
		Falling->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0, Gravity * 0.2, 0.001));
	TestTrue(TEXT("uncompensated displacement follows gravity within first-order error"), Matches(
		Falling->GetBodyInstance()->GetCOMPosition() / 100.0, FallStart + Gravity * 0.02,
		Gravity.Size() * 0.2 * Dt + 1.0e-4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLCOMVelocityIdentityTest,
	"UERL.Integration.PhysicsResponse.FreeBody.COMAndPointVelocityIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLCOMVelocityIdentityTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	auto Scene = MakeScene();
	UWorld* World = Scene->GetWorld();
	UStaticMeshComponent* Cube = World ? MakeCube(*World, 1.0) : nullptr;
	if (!Cube || !Cube->GetBodyInstance()) { AddError(TEXT("missing COM body")); return false; }
	Cube->SetCenterOfMass(FVector(5.0, -3.0, 2.0));
	if (!Tick(*this, *World, 0.005)) { return false; }
	FBodyInstance* Body = Cube->GetBodyInstance();
	Body->SetLinearVelocity(FVector(40.0, 20.0, -10.0), false);
	Body->SetAngularVelocityInRadians(FVector(0.3, 1.0, -0.5), false);
	if (!Tick(*this, *World, 0.005)) { return false; }
	const FVector Com = Body->GetCOMPosition();
	const FVector Velocity = Body->GetUnrealWorldVelocity() / 100.0;
	const FVector Angular = Body->GetUnrealWorldAngularVelocityInRadians();
	for (const FVector& OffsetMeters : { FVector::ZeroVector, FVector(0.1, -0.2, 0.3), FVector(-0.3, 0.2, 0.1) })
	{
		const FVector Expected = Velocity + FVector::CrossProduct(Angular, OffsetMeters);
		const FVector Actual = Body->GetUnrealWorldVelocityAtPoint(Com + OffsetMeters * 100.0) / 100.0;
		TestTrue(TEXT("point velocity includes angular motion around an offset COM"), Matches(Actual, Expected, 1.0e-5));
	}
	TestTrue(TEXT("the COM-offset fixture actually has an offset"),
		(Com - Body->GetUnrealWorldTransform().GetLocation()).Size() > 1.0);
	return true;
}

#endif

#include "Misc/AutomationTest.h"

#include "UERLGenericRobotContactListener.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotContactForceStateTest,
	"UERL.Unit.Robot.GenericContact.ForceObjectState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotContactForceStateTest::RunTest(const FString& Parameters)
{
	const auto Shared = EUERLEnvironmentCollisionScope::SharedWorld;
	const auto Isolated = EUERLEnvironmentCollisionScope::SlotIsolated;
	TestTrue(TEXT("static terrain contributes shared-world force"),
		IsGenericRobotContactForceStateAccepted(Shared, Chaos::EObjectStateType::Static));
	TestTrue(TEXT("movable non-simulated terrain contributes shared-world force"),
		IsGenericRobotContactForceStateAccepted(Shared, Chaos::EObjectStateType::Kinematic));
	TestFalse(TEXT("dynamic other bodies remain excluded in shared world"),
		IsGenericRobotContactForceStateAccepted(Shared, Chaos::EObjectStateType::Dynamic));
	TestFalse(TEXT("sleeping simulated bodies remain excluded in shared world"),
		IsGenericRobotContactForceStateAccepted(Shared, Chaos::EObjectStateType::Sleeping));
	for (const auto State : {Chaos::EObjectStateType::Static, Chaos::EObjectStateType::Kinematic,
		Chaos::EObjectStateType::Dynamic, Chaos::EObjectStateType::Sleeping})
	{
		TestTrue(TEXT("isolated force accepts every non-self environment state"),
			IsGenericRobotContactForceStateAccepted(Isolated, State));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotContactFilterTest,
	"UERL.Unit.Robot.GenericContact.AC_UE_UNIT_ROBOT_CONTACT_002.EnvironmentFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotContactFilterTest::RunTest(const FString& Parameters)
{
	FHitResult BlockingHit;
	BlockingHit.bBlockingHit = true;
	TestTrue(TEXT("blocking hit from a Slot Environment actor is contact"),
		IsGenericRobotEnvironmentHit(
			BlockingHit, EUERLEnvironmentCollisionScope::SlotIsolated, ECC_GameTraceChannel1, true));
	TestFalse(TEXT("another Slot's blocking hit is not contact"),
		IsGenericRobotEnvironmentHit(
			BlockingHit, EUERLEnvironmentCollisionScope::SlotIsolated, ECC_GameTraceChannel2, false));
	TestTrue(TEXT("blocking WorldStatic hit is shared-world ground contact"),
		IsGenericRobotEnvironmentHit(
			BlockingHit, EUERLEnvironmentCollisionScope::SharedWorld, ECC_WorldStatic, false));
	TestFalse(TEXT("blocking PhysicsBody hit is not shared-world ground contact"),
		IsGenericRobotEnvironmentHit(
			BlockingHit, EUERLEnvironmentCollisionScope::SharedWorld, ECC_PhysicsBody, false));

	FHitResult Overlap;
	Overlap.bBlockingHit = false;
	TestFalse(TEXT("non-blocking Environment overlap is not contact"),
		IsGenericRobotEnvironmentHit(
			Overlap, EUERLEnvironmentCollisionScope::SlotIsolated, ECC_GameTraceChannel1, true));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-CONTACT-002: contact accepts owned isolated ground or shared WorldStatic only"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotContactBodyMappingTest,
	"UERL.Unit.Robot.GenericContact.BodyMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotContactBodyMappingTest::RunTest(const FString& Parameters)
{
	const TMap<FName, int32> BodyIndices = {
		{ FName(TEXT("cart")), 0 },
		{ FName(TEXT("pole")), 1 },
	};
	int32 BodyIndex = INDEX_NONE;
	TestTrue(TEXT("known hit bone resolves to body index"),
		ResolveGenericRobotContactBodyIndex(BodyIndices, FName(TEXT("pole")), BodyIndex));
	TestEqual(TEXT("known body index is preserved"), BodyIndex, 1);
	TestFalse(TEXT("unknown hit bone is ignored"),
		ResolveGenericRobotContactBodyIndex(BodyIndices, FName(TEXT("unknown")), BodyIndex));
	TestEqual(TEXT("unknown body leaves no usable index"), BodyIndex, INDEX_NONE);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-CONTACT-003: hit bone mapping is explicit and unknown bones are ignored"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotContactStateTest,
	"UERL.Unit.Robot.GenericContact.StateLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotContactStateTest::RunTest(const FString& Parameters)
{
	FGenericRobotContactState State(3);
	TestFalse(TEXT("body 0 starts without contact"), State.HasContact(0));
	TestFalse(TEXT("body 2 starts without contact"), State.HasContact(2));

	State.MarkBody(2);
	TestTrue(TEXT("a hit marks only its body"), State.HasContact(2));
	TestFalse(TEXT("another body remains clear"), State.HasContact(0));

	TestTrue(TEXT("100 centimetre-kilogram-per-second impulse at 0.01 seconds is 100 newtons"),
		FMath::IsNearlyEqual(
			ConvertGenericRobotAccumulatedImpulseToForceNewtons(FVector(100.0, 0.0, 0.0), 0.01),
			100.0f,
			1.0e-4f));
	TestTrue(TEXT("zero impulse produces zero force"),
		FMath::IsNearlyZero(
			ConvertGenericRobotAccumulatedImpulseToForceNewtons(FVector::ZeroVector, 0.01),
			1.0e-6f));

	State.RecordContactForce(2, 12.0f);
	State.RecordContactForce(2, 8.0f);
	TestTrue(TEXT("contact force keeps the latest solver-step sample"),
		FMath::IsNearlyEqual(State.ContactForceNewtons(2), 8.0f, 1.0e-6f));
	State.RecordContactForce(2, 20.0f);
	TestTrue(TEXT("a later solver-step sample replaces the previous force"),
		FMath::IsNearlyEqual(State.ContactForceNewtons(2), 20.0f, 1.0e-6f));

	State.Clear();
	State.BeginPhysicsSample();
	State.MarkBodySupported(2);
	TestTrue(TEXT("the completed sample reports geometric support"),
		FMath::IsNearlyEqual(State.SupportValue(2), 1.0f, 1.0e-6f));
	State.BeginPhysicsSample();
	TestTrue(TEXT("a later unsupported sample replaces early-window support"),
		FMath::IsNearlyEqual(State.SupportValue(2), 0.0f, 1.0e-6f));
	TestTrue(TEXT("an unsupported body has zero support value"),
		FMath::IsNearlyEqual(State.SupportValue(0), 0.0f, 1.0e-6f));

	State.Clear();
	TestFalse(TEXT("collect/reset boundary clears the event flag"), State.HasContact(2));
	TestTrue(TEXT("collect/reset boundary clears terminal support"),
		FMath::IsNearlyEqual(State.SupportValue(2), 0.0f, 1.0e-6f));
	TestTrue(TEXT("collect/reset boundary clears latest contact force"),
		FMath::IsNearlyEqual(State.ContactForceNewtons(2), 0.0f, 1.0e-6f));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-CONTACT-001: terminal support and latest solver-step force replace window accumulation and clear at State boundary"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotTerminalContactContractTest,
	"UERL.Integration.Policy.Contact.AC_UE_INT_POLICY_CONTACT_001.TerminalSupport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotTerminalContactContractTest::RunTest(const FString& Parameters)
{
	for (const int32 PhysicsTicks : { 1, 4, 7 })
	{
		FGenericRobotContactState State(1);
		for (int32 Tick = 0; Tick < PhysicsTicks - 1; ++Tick)
		{
			State.BeginPhysicsSample();
			State.MarkBodySupported(0);
		}
		State.BeginPhysicsSample();
		TestTrue(
			FString::Printf(TEXT("early support is discarded at terminal sample for N=%d"), PhysicsTicks),
			FMath::IsNearlyZero(State.SupportValue(0), 1.0e-6f));

		State.BeginPhysicsSample();
		State.MarkBodySupported(0);
		TestTrue(
			FString::Printf(TEXT("terminal support is one for N=%d"), PhysicsTicks),
			FMath::IsNearlyEqual(State.SupportValue(0), 1.0f, 1.0e-6f));
	}
	AddInfo(TEXT("[VERIFY] AC_UE_INT_POLICY_CONTACT_001/002: support is sampled once at the terminal boundary, independent of N"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotSolverContactContractTest,
	"UERL.Integration.Policy.Contact.AC_UE_INT_POLICY_CONTACT_005_006.SolverStepForce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotSolverContactContractTest::RunTest(const FString& Parameters)
{
	constexpr float ForceNewtons = 100.0f;
	for (const double SolverStepSeconds : { 0.005, 0.01, 1.0 / 60.0 })
	{
		const FVector Impulse(100.0 * ForceNewtons * SolverStepSeconds, 0.0, 0.0);
		TestTrue(
			FString::Printf(TEXT("fixed force converts at solver dt %.6f"), SolverStepSeconds),
			FMath::IsNearlyEqual(
				ConvertGenericRobotAccumulatedImpulseToForceNewtons(Impulse, SolverStepSeconds),
				ForceNewtons,
				1.0e-3f));
	}
	const FVector FixedImpulse(100.0, 0.0, 0.0);
	const float FastForce = ConvertGenericRobotAccumulatedImpulseToForceNewtons(FixedImpulse, 0.005);
	const float SlowForce = ConvertGenericRobotAccumulatedImpulseToForceNewtons(FixedImpulse, 0.01);
	TestTrue(TEXT("fixed impulse force follows inverse solver dt"), FMath::IsNearlyEqual(FastForce, SlowForce * 2.0f, 1.0e-3f));
	AddInfo(TEXT("[VERIFY] AC_UE_INT_POLICY_CONTACT_005/006: the same solver-step impulse/dt conversion is shared by training and deployment"));
	return true;
}

#endif

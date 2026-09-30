#include "Misc/AutomationTest.h"

#include "UERLActuatorLaw.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UERLRobot;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLActuatorLawPositionTest,
	"UERL.Unit.Robot.ActuatorLaw.PositionPD",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLActuatorLawPositionTest::RunTest(const FString& Parameters)
{
	const FUERLActuatorLawConfig Config{ 10.0, 2.0, 100.0 };
	const double Effort = ComputeUnifiedActuatorEffort(Config, 1.0, 0.25, 0.5);

	TestTrue(TEXT("position PD effort is correct"), FMath::IsNearlyEqual(Effort, 6.5));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ACTUATOR-001: position PD uses target position and measured velocity"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLActuatorLawEffortTest,
	"UERL.Unit.Robot.ActuatorLaw.EffortFeedForward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLActuatorLawEffortTest::RunTest(const FString& Parameters)
{
	const FUERLActuatorLawConfig Config{ 0.0, 5.0, 100.0 };
	const double Effort = ComputeUnifiedActuatorEffort(Config, 3.0, 99.0, 0.2);

	TestTrue(TEXT("zero-stiffness target is effort feed-forward"), FMath::IsNearlyEqual(Effort, 2.0));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ACTUATOR-002: kp=0 target shares the effort feed-forward path"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLActuatorLawPassiveAndClampTest,
	"UERL.Unit.Robot.ActuatorLaw.PassiveAndClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLActuatorLawPassiveAndClampTest::RunTest(const FString& Parameters)
{
	const FUERLActuatorLawConfig Passive{ 0.0, 0.0, 10.0 };
	TestTrue(
		TEXT("zero gains produce no effort"),
		FMath::IsNearlyEqual(ComputeUnifiedActuatorEffort(Passive, 100.0, -20.0, 5.0), 0.0));

	const FUERLActuatorLawConfig Limited{ 10.0, 0.0, 3.0 };
	TestTrue(
		TEXT("positive effort is clamped"),
		FMath::IsNearlyEqual(ComputeUnifiedActuatorEffort(Limited, 1.0, -1.0, 0.0), 3.0));
	TestTrue(
		TEXT("negative effort is clamped"),
		FMath::IsNearlyEqual(ComputeUnifiedActuatorEffort(Limited, -1.0, 1.0, 0.0), -3.0));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ACTUATOR-003: passive output and effort limits are unified"));
	return true;
}

#endif

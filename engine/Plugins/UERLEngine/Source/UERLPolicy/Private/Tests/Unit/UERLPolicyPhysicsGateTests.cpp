#include "Misc/AutomationTest.h"

#include "UERLPolicyPhysicsGate.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FUERLPhysicsSnapshot GoodSnapshot()
	{
		FUERLPhysicsSnapshot Snapshot;
		Snapshot.SolverPath = FName(TEXT("Chaos"));
		Snapshot.bHasSolver = true;
		Snapshot.bSubstepping = true;
		Snapshot.MaxSubstepDeltaTime = 0.005;
		Snapshot.MaxSubsteps = 7;
		Snapshot.MinPhysicsDeltaTime = 0.0;
		Snapshot.MaxPhysicsDeltaTime = 1.0 / 30.0;
		Snapshot.WorldTimeDilation = 1.0;
		Snapshot.OwnerTimeDilation = 1.0;
		Snapshot.bOwnerTimeDilationKnown = true;
		Snapshot.GravityZ = -980.0;
		return Snapshot;
	}

	FUERLPolicyArtifactTiming GoodTiming()
	{
		FUERLPolicyArtifactTiming Timing;
		Timing.PhysicsDt = 0.005;
		Timing.DecimationMin = 1;
		Timing.DecimationMax = 7;
		return Timing;
	}

	const FUERLPhysicsGateItem* FindDiagnostic(
		const FUERLPhysicsGateReport& Report,
		FName Item)
	{
		return Report.Diagnostics.FindByPredicate([Item](const FUERLPhysicsGateItem& Entry)
		{
			return Entry.Item == Item;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPhysicsGateBaselineTest,
	"UERL.Unit.Policy.PhysicsGate.AC_UE_UNIT_BASELINE_001.SynchronousDeploymentPasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPhysicsGateBaselineTest::RunTest(const FString& Parameters)
{
	const FUERLPhysicsGateReport Report = CheckDeployPhysicsRequirements(GoodSnapshot(), GoodTiming());
	TestTrue(TEXT("synchronous deployment baseline passes"), Report.bPassed);
	TestTrue(TEXT("baseline has no failures"), Report.Failures.IsEmpty());
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-BASELINE-001: synchronous Chaos substep deployment baseline passes"));
	return Report.bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPhysicsGateSubstepRequiredTest,
	"UERL.Unit.Policy.PhysicsGate.AC_UE_UNIT_BASELINE_002.SubsteppingRequired",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPhysicsGateSubstepRequiredTest::RunTest(const FString& Parameters)
{
	FUERLPhysicsSnapshot Snapshot = GoodSnapshot();
	Snapshot.bSubstepping = false;
	const FUERLPhysicsGateReport Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	const FUERLPhysicsGateItem* Failure = Report.FindFailure(TEXT("bSubstepping"));
	TestFalse(TEXT("disabled synchronous substepping is rejected"), Report.bPassed);
	TestNotNull(TEXT("substepping report names the item"), Failure);
	if (Failure)
	{
		TestEqual(TEXT("report actual value"), Failure->Actual, FString(TEXT("false")));
		TestEqual(TEXT("report required value"), Failure->Requirement, FString(TEXT("true")));
		TestTrue(TEXT("report includes a concrete settings location"), Failure->Change.Contains(TEXT("Project Settings")));
	}
	TestTrue(TEXT("formatted report carries actual/requirement/change"),
		Report.ToString().Contains(TEXT("actual=false"))
			&& Report.ToString().Contains(TEXT("requirement=true"))
			&& Report.ToString().Contains(TEXT("change=Project Settings")));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-BASELINE-002: disabled synchronous substepping reports an actionable failure"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPhysicsGateCapacityAndDilationTest,
	"UERL.Unit.Policy.PhysicsGate.AC_UE_UNIT_BASELINE_003.CapacityDilationAndFloatBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPhysicsGateCapacityAndDilationTest::RunTest(const FString& Parameters)
{
	FUERLPhysicsSnapshot Snapshot = GoodSnapshot();
	Snapshot.MaxSubstepDeltaTime = 0.0;
	FUERLPhysicsGateReport Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestNotNull(TEXT("illegal substep dt is rejected"), Report.FindFailure(TEXT("MaxSubstepDeltaTime")));

	Snapshot = GoodSnapshot();
	Snapshot.MaxSubsteps = 6;
	Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestNotNull(TEXT("insufficient substep capacity is rejected"), Report.FindFailure(TEXT("MaxSubsteps")));

	Snapshot = GoodSnapshot();
	Snapshot.WorldTimeDilation = 0.5;
	Snapshot.OwnerTimeDilation = 0.5;
	Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestNotNull(TEXT("world dilation is rejected"), Report.FindFailure(TEXT("world_time_dilation")));
	TestNotNull(TEXT("owner dilation is rejected"), Report.FindFailure(TEXT("owner_time_dilation")));

	// These decimal settings are represented by float in UE PhysicsSettings.
	// The comparison tolerance must not turn 35ms / 5ms into an eighth step.
	Snapshot = GoodSnapshot();
	Snapshot.MaxSubstepDeltaTime = 0.005f;
	Snapshot.MaxSubsteps = 7;
	Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestTrue(TEXT("35ms/5ms float boundary passes with seven substeps"), Report.bPassed);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-BASELINE-003: invalid h/capacity/dilation fail and float boundary remains exact"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPhysicsGateAsyncSolverTest,
	"UERL.Unit.Policy.PhysicsGate.AC_UE_UNIT_BASELINE_004.AsyncAndSolverPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPhysicsGateAsyncSolverTest::RunTest(const FString& Parameters)
{
	FUERLPhysicsSnapshot Snapshot = GoodSnapshot();
	Snapshot.bTickPhysicsAsync = true;
	FUERLPhysicsGateReport Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestNotNull(TEXT("async physics tick is rejected"), Report.FindFailure(TEXT("bTickPhysicsAsync")));

	Snapshot = GoodSnapshot();
	Snapshot.bSubsteppingAsync = true;
	Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestNotNull(TEXT("async substepping is rejected"), Report.FindFailure(TEXT("bSubsteppingAsync")));

	Snapshot = GoodSnapshot();
	Snapshot.SolverPath = FName(TEXT("Unsupported"));
	Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestNotNull(TEXT("unsupported solver path is rejected"), Report.FindFailure(TEXT("solver_path")));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-BASELINE-004: async switches and unsupported solver paths are rejected"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPhysicsGateReadOnlyTest,
	"UERL.Unit.Policy.PhysicsGate.AC_UE_UNIT_BASELINE_005.ReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPhysicsGateReadOnlyTest::RunTest(const FString& Parameters)
{
	const FUERLPhysicsSnapshot Before = GoodSnapshot();
	FUERLPhysicsSnapshot After = Before;
	const FUERLPhysicsGateReport Report = CheckDeployPhysicsRequirements(After, GoodTiming());
	TestTrue(TEXT("baseline still passes"), Report.bPassed);
	TestEqual(TEXT("solver path is unchanged"), After.SolverPath, Before.SolverPath);
	TestEqual(TEXT("substep flag is unchanged"), After.bSubstepping, Before.bSubstepping);
	TestEqual(TEXT("substep dt is unchanged"), After.MaxSubstepDeltaTime, Before.MaxSubstepDeltaTime);
	TestEqual(TEXT("substep capacity is unchanged"), After.MaxSubsteps, Before.MaxSubsteps);
	TestEqual(TEXT("min dt is unchanged"), After.MinPhysicsDeltaTime, Before.MinPhysicsDeltaTime);
	TestEqual(TEXT("world dilation is unchanged"), After.WorldTimeDilation, Before.WorldTimeDilation);
	TestEqual(TEXT("owner dilation is unchanged"), After.OwnerTimeDilation, Before.OwnerTimeDilation);
	TestEqual(TEXT("gravity is unchanged"), After.GravityZ, Before.GravityZ);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-BASELINE-005: deployment gate is read-only"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPhysicsGateTrainingBaselineTest,
	"UERL.Unit.Policy.PhysicsGate.AC_UE_UNIT_BASELINE_006.TrainingBaselineRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPhysicsGateTrainingBaselineTest::RunTest(const FString& Parameters)
{
	FUERLPhysicsSnapshot TrainingSnapshot = GoodSnapshot();
	TrainingSnapshot.bSubstepping = false;
	const FUERLPhysicsGateReport Report = CheckDeployPhysicsRequirements(TrainingSnapshot, GoodTiming());
	TestFalse(TEXT("training fixed-frame baseline cannot satisfy deployment gate"), Report.bPassed);
	TestNotNull(TEXT("training baseline failure names synchronous substepping"), Report.FindFailure(TEXT("bSubstepping")));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-BASELINE-006: training gate remains separate from deployment synchronous-substep requirements"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPhysicsGateMaxDeltaTest,
	"UERL.Unit.Policy.PhysicsGate.AC_UE_UNIT_BASELINE_007.MaxPhysicsDeltaIsDiagnostic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPhysicsGateMaxDeltaTest::RunTest(const FString& Parameters)
{
	FUERLPhysicsSnapshot Snapshot = GoodSnapshot();
	Snapshot.MaxPhysicsDeltaTime = 1.0 / 30.0;
	FUERLPhysicsGateReport Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestTrue(TEXT("default 1/30 max physics dt does not reject valid substeps"), Report.bPassed);
	TestNotNull(TEXT("max physics dt is reported as a diagnostic"), FindDiagnostic(Report, TEXT("MaxPhysicsDeltaTime")));

	Snapshot.MaxSubsteps = 6;
	Report = CheckDeployPhysicsRequirements(Snapshot, GoodTiming());
	TestFalse(TEXT("changing substep capacity changes the gate"), Report.bPassed);
	TestNotNull(TEXT("capacity failure is explicit"), Report.FindFailure(TEXT("MaxSubsteps")));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-BASELINE-007: MaxPhysicsDeltaTime is diagnostic while substep capacity remains decisive"));
	return true;
}

#endif

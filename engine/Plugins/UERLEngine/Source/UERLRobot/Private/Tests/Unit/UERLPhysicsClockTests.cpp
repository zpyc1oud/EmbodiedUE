#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "PreviewScene.h"
#include "UERLPhysicsSnapshot.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPhysicsClockIntegrationTest,
	"UERL.Integration.Policy.Clock.AC_UE_INT_CLOCK_001.CompletedSolverClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhysicsClockIntegrationTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();
	TestNotNull(TEXT("clock integration World created"), World);
	if (!World)
	{
		return false;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	AStaticMeshActor* Body = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator);
	TestNotNull(TEXT("clock integration dynamic body created"), Body);
	if (!Cube || !Body)
	{
		return false;
	}
	UStaticMeshComponent* BodyComponent = Body->GetStaticMeshComponent();
	BodyComponent->SetStaticMesh(Cube);
	BodyComponent->SetMobility(EComponentMobility::Movable);
	BodyComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	BodyComponent->SetSimulatePhysics(true);
	BodyComponent->SetCollisionObjectType(ECC_PhysicsBody);

	FString Error;
	FUERLPhysicsSnapshot PhysicsSnapshot;
	TestTrue(TEXT("World physics snapshot is readable"),
		ReadUERLPhysicsSnapshot(*World, Body, PhysicsSnapshot, Error));
	TestTrue(TEXT("snapshot identifies the active Chaos solver"),
		PhysicsSnapshot.bHasSolver && PhysicsSnapshot.SolverPath == FName(TEXT("Chaos")));
	TestTrue(TEXT("snapshot captures the supplied owner dilation"),
		PhysicsSnapshot.bOwnerTimeDilationKnown
			&& FMath::IsNearlyEqual(PhysicsSnapshot.OwnerTimeDilation, 1.0, 1.0e-6));

	FUERLSolverClockSnapshot Before;
	TestTrue(TEXT("initial solver clock is readable"), ReadUERLSolverClock(*World, Before, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	World->Tick(ELevelTick::LEVELTICK_All, 0.034f);
	++GFrameCounter;
	FUERLSolverClockSnapshot After34;
	TestTrue(TEXT("34ms completed solver clock is readable"), ReadUERLSolverClock(*World, After34, Error));
	const double Delta34 = After34.SolverTime - Before.SolverTime;
	TestTrue(TEXT("34ms tick advances a real solver frame"), After34.Frame > Before.Frame);
	TestTrue(TEXT("34ms tick advances positive solver time"), Delta34 > 0.0);
	TestTrue(TEXT("34ms last completed dt is positive"), After34.LastDt > 0.0);
	TestTrue(TEXT("34ms solver time does not exceed requested game delta"), Delta34 <= 0.034 + 1.0e-4);
	TestTrue(TEXT("34ms total solver time includes its last completed step"), Delta34 + 1.0e-6 >= After34.LastDt);

	World->Tick(ELevelTick::LEVELTICK_All, 0.2f);
	++GFrameCounter;
	FUERLSolverClockSnapshot AfterLong;
	TestTrue(TEXT("long-frame solver clock is readable"), ReadUERLSolverClock(*World, AfterLong, Error));
	const double LongDelta = AfterLong.SolverTime - After34.SolverTime;
	TestTrue(TEXT("long frame advances a real solver frame"), AfterLong.Frame > After34.Frame);
	TestTrue(TEXT("long frame has a positive completed dt"), AfterLong.LastDt > 0.0);
	TestTrue(TEXT("long frame reports completed time instead of configured request"), LongDelta > 0.0 && LongDelta <= 0.2 + 1.0e-4);
	TestTrue(TEXT("long frame total includes its last completed step"), LongDelta + 1.0e-6 >= AfterLong.LastDt);

	FUERLSolverClockSnapshot ShortStart = AfterLong;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		World->Tick(ELevelTick::LEVELTICK_All, 0.008f);
		++GFrameCounter;
	}
	FUERLSolverClockSnapshot AfterShorts;
	TestTrue(TEXT("short-frame solver clock is readable"), ReadUERLSolverClock(*World, AfterShorts, Error));
	const double ShortDelta = AfterShorts.SolverTime - ShortStart.SolverTime;
	TestTrue(TEXT("multiple short frames make measurable solver progress"), AfterShorts.Frame > ShortStart.Frame);
	TestTrue(TEXT("multiple short frames report positive completed time"), ShortDelta > 0.0);
	TestTrue(TEXT("multiple short frames stay within their total request"), ShortDelta <= 3.0 * 0.008 + 1.0e-4);
	TestTrue(TEXT("multiple short frames include the final completed step"), ShortDelta + 1.0e-6 >= AfterShorts.LastDt);

	const FUERLSolverClockSnapshot BeforePause = AfterShorts;
	World->Tick(ELevelTick::LEVELTICK_TimeOnly, 0.1f);
	++GFrameCounter;
	FUERLSolverClockSnapshot AfterPause;
	TestTrue(TEXT("paused clock remains readable"), ReadUERLSolverClock(*World, AfterPause, Error));
	TestEqual(TEXT("paused frame does not fabricate a solver frame"), AfterPause.Frame, BeforePause.Frame);
	TestTrue(TEXT("paused time does not fabricate solver progress"),
		FMath::IsNearlyEqual(AfterPause.SolverTime, BeforePause.SolverTime, 1.0e-8));

	AddInfo(FString::Printf(
		TEXT("[VERIFY] AC-UE-INT-CLOCK-001: completed solver deltas 34ms=%.6f long=%.6f short_total=%.6f; pause delta=%.6f"),
		Delta34,
		LongDelta,
		ShortDelta,
		AfterPause.SolverTime - BeforePause.SolverTime));
	return true;
}

#endif

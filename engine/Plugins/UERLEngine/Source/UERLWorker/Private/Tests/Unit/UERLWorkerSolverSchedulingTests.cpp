#include "Misc/AutomationTest.h"

#include "Engine/World.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PreviewScene.h"
#include "UERLPhysicsSnapshot.h"
#include "UERLWorkerSolverScheduling.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLWorkerSolverSchedulingLifecycleTest,
	"UERL.Unit.Worker.SolverSchedulingLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLWorkerSolverSchedulingLifecycleTest::RunTest(const FString& Parameters)
{
	const auto MakeScene = []()
	{
		return MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false).SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true).SetTransactional(false).SetEditor(false));
	};
	TUniquePtr<FPreviewScene> First = MakeScene();
	TUniquePtr<FPreviewScene> Other = MakeScene();
	UWorld* World = First->GetWorld();
	UWorld* OtherWorld = Other->GetWorld();
	if (!TestNotNull(TEXT("first World exists"), World)
		|| !TestNotNull(TEXT("other World exists"), OtherWorld)
		|| !TestNotNull(TEXT("first physics Scene exists"), World->GetPhysicsScene())
		|| !TestNotNull(TEXT("other physics Scene exists"), OtherWorld->GetPhysicsScene()))
	{
		return false;
	}
	Chaos::FPhysicsSolver* Solver = World->GetPhysicsScene()->GetSolver();
	Chaos::FPhysicsSolver* OtherSolver = OtherWorld->GetPhysicsScene()->GetSolver();
	if (!TestNotNull(TEXT("first Scene has a solver"), Solver)
		|| !TestNotNull(TEXT("other Scene has a solver"), OtherSolver))
	{
		return false;
	}
	const auto OriginalMode = Solver->GetThreadingMode();
	const auto OtherMode = OtherSolver->GetThreadingMode();
	FUERLPhysicsSnapshot Before;
	FString Error;
	if (!TestTrue(TEXT("initial physics settings can be read"),
		ReadUERLPhysicsSnapshot(*World, nullptr, Before, Error)))
	{
		return false;
	}
	{
		FUERLWorkerSolverScheduling Scheduling;
		if (!TestTrue(TEXT("outer scheduling acquired"), Scheduling.Acquire(*World, Error)))
		{
			return false;
		}
		TestTrue(TEXT("only the selected solver advances on the calling thread"),
			Solver->GetThreadingMode() == Chaos::EThreadingModeTemp::SingleThread);
		TestTrue(TEXT("the other World retains its scheduling"), OtherSolver->GetThreadingMode() == OtherMode);
		FUERLPhysicsSnapshot During;
		if (!TestTrue(TEXT("physics settings remain readable"),
			ReadUERLPhysicsSnapshot(*World, nullptr, During, Error)))
		{
			return false;
		}
		TestEqual(TEXT("async physics setting unchanged"), During.bTickPhysicsAsync, Before.bTickPhysicsAsync);
		TestEqual(TEXT("substep setting unchanged"), During.bSubstepping, Before.bSubstepping);
		TestEqual(TEXT("async substep setting unchanged"), During.bSubsteppingAsync, Before.bSubsteppingAsync);
		TestEqual(TEXT("substep dt unchanged"), During.MaxSubstepDeltaTime, Before.MaxSubstepDeltaTime);
		TestEqual(TEXT("substep count unchanged"), During.MaxSubsteps, Before.MaxSubsteps);
		TestEqual(TEXT("maximum dt unchanged"), During.MaxPhysicsDeltaTime, Before.MaxPhysicsDeltaTime);
		TestEqual(TEXT("gravity unchanged"), During.GravityZ, Before.GravityZ);
		Scheduling.Reset();
		TestTrue(TEXT("explicit release restores the original mode"), Solver->GetThreadingMode() == OriginalMode);
		TestTrue(TEXT("scope can be reacquired"), Scheduling.Acquire(*World, Error));
	}
	TestTrue(TEXT("scope destruction restores the original mode"), Solver->GetThreadingMode() == OriginalMode);
	TestTrue(TEXT("another World remains unchanged after release"), OtherSolver->GetThreadingMode() == OtherMode);

	// Destruction of the selected World must not access its old solver or affect a survivor.
	{
		FUERLWorkerSolverScheduling Scheduling;
		TestTrue(TEXT("scope acquires before World teardown"), Scheduling.Acquire(*World, Error));
		First.Reset();
		Scheduling.Reset();
	}
	TestTrue(TEXT("World teardown does not change the surviving solver"), OtherSolver->GetThreadingMode() == OtherMode);
	return true;
}

#endif

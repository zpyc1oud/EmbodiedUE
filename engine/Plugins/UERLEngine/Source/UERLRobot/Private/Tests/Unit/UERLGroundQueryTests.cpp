#include "Misc/AutomationTest.h"
#include "UERLGroundQuery.h"
#include "UERLRayGroundInput.h"
#include "UERLSlotCollisionPlan.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "PreviewScene.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UERLGroundQueryTest
{
	AActor* SpawnWorldStaticBox(UWorld& World, const FVector& Location, const FVector& Scale)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube)
		{
			return nullptr;
		}
		AStaticMeshActor* Box = World.SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator);
		if (!Box)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = Box->GetStaticMeshComponent();
		Component->SetStaticMesh(Cube);
		Component->SetMobility(EComponentMobility::Static);
		Component->SetCollisionObjectType(ECC_WorldStatic);
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetWorldScale3D(Scale);
		return Box;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGroundQueryBindingsTest,
	"UERL.Unit.Robot.GroundQuery.Bindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGroundQueryBindingsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();
	if (!TestNotNull(TEXT("query fixture world exists"), World))
	{
		return false;
	}
	// The engine cube is 100 cm wide. These top surfaces are exactly 0 and 50 cm.
	AActor* Floor = UERLGroundQueryTest::SpawnWorldStaticBox(*World, FVector(0.0, 0.0, -25.0), FVector(10.0, 10.0, 0.5));
	AActor* Step = UERLGroundQueryTest::SpawnWorldStaticBox(*World, FVector(75.0, 0.0, 25.0), FVector(1.0, 1.0, 0.5));
	if (!TestNotNull(TEXT("floor exists"), Floor) || !TestNotNull(TEXT("step exists"), Step))
	{
		return false;
	}
	FUERLSlotCollisionPlan Plan;
	FString Error;
	if (!TestTrue(TEXT("shared query profile compiles"),
		Plan.Compile(1, EUERLEnvironmentCollisionScope::SharedWorld, Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<TWeakObjectPtr<AActor>> Owners = { Floor };
	TArray<FHitResult> Scratch;
	FUERLGroundQueryContext Query;
	Query.CollisionProfile = Plan.Profile(0);
	Query.PermittedActors = &Owners;
	Query.ScratchHits = &Scratch;
	const FVector Start(75.0, 0.0, 200.0);
	const FVector End(75.0, 0.0, -200.0);
	FHitResult Hit;
	if (!TestTrue(TEXT("owned floor query succeeds without Robot topology"),
		QueryUERLGroundHit(Query, *World, Start, End, TEXT("binding-test"), Hit, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("owner filter excludes the higher step"), Hit.GetActor() == Floor);
	TestTrue(TEXT("owned floor height is zero"), FMath::IsNearlyEqual(Hit.ImpactPoint.Z, 0.0, 1.0e-3));

	Query.Purpose = EUERLTerrainQueryPurpose::DeploymentWorldStatic;
	if (!TestTrue(TEXT("explicit WorldStatic binding succeeds"),
		QueryUERLGroundHit(Query, *World, Start, End, TEXT("binding-test"), Hit, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("WorldStatic binding selects the higher step"), Hit.GetActor() == Step);
	TestTrue(TEXT("step height is fifty centimetres"), FMath::IsNearlyEqual(Hit.ImpactPoint.Z, 50.0, 1.0e-3));

	Query.IgnoredActor = Step;
	if (!TestTrue(TEXT("ignored actor is excluded from the same query"),
		QueryUERLGroundHit(Query, *World, Start, End, TEXT("binding-test"), Hit, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("ignored step reveals the floor"), Hit.GetActor() == Floor);
	TestTrue(TEXT("ignored step leaves floor height unchanged"), FMath::IsNearlyEqual(Hit.ImpactPoint.Z, 0.0, 1.0e-3));

	Query.Purpose = EUERLTerrainQueryPurpose::TrainingOwned;
	Owners.Reset();
	TestFalse(TEXT("empty ownership never widens to WorldStatic"),
		QueryUERLGroundHit(Query, *World, Start, End, TEXT("binding-test"), Hit, Error));
	TestTrue(TEXT("missing owner diagnostic is retained"), Error.Contains(TEXT("no training terrain owner")));
	Owners.Add(Floor);
	Error.Reset();
	TestTrue(TEXT("restoring valid ownership permits a new query"),
		QueryUERLGroundHit(Query, *World, Start, End, TEXT("binding-test"), Hit, Error));
	TestFalse(TEXT("a disjoint empty region fails instead of reusing an old hit"),
		QueryUERLGroundHit(Query, *World, FVector(2000.0, 0.0, 200.0),
			FVector(2000.0, 0.0, -200.0), TEXT("binding-test"), Hit, Error));
	TestTrue(TEXT("no-hit diagnostic is retained"), Error.Contains(TEXT("no permitted WorldStatic ground")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLConfiguredRayTest, "UERL.Unit.Robot.GroundQuery.DeclaredProvider",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLConfiguredRayTest::RunTest(const FString&)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues()
		.SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(true)
		.SetTransactional(false).SetEditor(false));
	UWorld* World = Scene->GetWorld();
	if (!TestNotNull(TEXT("world"), World)) { return false; }
	FUERLSlotCollisionPlan Plan; FString Error;
	if (!TestTrue(TEXT("shared profiles"), Plan.Compile(2, EUERLEnvironmentCollisionScope::SharedWorld, Error)))
	{ return false; }
	TArray<FUERLInputSlotBinding> Bindings;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const double X = Index * 2000.0;
		AActor* Floor = UERLGroundQueryTest::SpawnWorldStaticBox(*World, FVector(X, 0.0, -25.0), FVector(10.0, 10.0, 0.5));
		AActor* Step = UERLGroundQueryTest::SpawnWorldStaticBox(*World, FVector(X+75.0, 0.0, 25.0), FVector(1.0, 1.0, 0.5));
		AActor* Excluded = UERLGroundQueryTest::SpawnWorldStaticBox(*World, FVector(X, 0.0, 150.0), FVector(10.0, 10.0, 0.1));
		if (!TestNotNull(TEXT("floor"), Floor) || !TestNotNull(TEXT("step"), Step)
			|| !TestNotNull(TEXT("excluded overhead"), Excluded)) { return false; }
		FUERLInputSlotBinding Binding;
		Binding.Slot.SlotId = Index; Binding.Slot.CollisionProfile = Plan.Profile(Index); Binding.World = World;
		Binding.Slot.TerrainQueryPurpose = Index == 0 ? EUERLTerrainQueryPurpose::TrainingOwned
			: EUERLTerrainQueryPurpose::DeploymentWorldStatic;
		Binding.SceneBindings.Add(TEXT("ground"), {Floor, Step});
		Binding.TransformReaders.Add(TEXT("root"), [X](FTransform& Transform, FString&)
		{
			Transform = FTransform(FQuat::Identity, FVector(X, 0.0, 100.0)); return true;
		});
		Bindings.Add(MoveTemp(Binding));
	}
	FUERLInputSpec Spec; Spec.Name = TEXT("scan"); Spec.ProviderId = TEXT("uerl.ray_ground");
	Spec.Attachment = TEXT("root"); Spec.ParametersJson = TEXT("{\"offsets_m\":[[0,0],[0.75,0]]}");
	FUERLInputSpec Clearance = Spec; Clearance.Name = TEXT("clearance");
	Clearance.ParametersJson = TEXT("{\"output\":\"clearance\"}");
	const TArray<FUERLInputSpec> Specs = {Spec, Clearance};
	FUERLInputRegistry Registry;
	if (!TestTrue(TEXT("register rays"), Registry.RegisterFactory(MakeUERLRayGroundInputFactory(), Error))) { return false; }
	FUERLCompiledInputSet Inputs;
	if (!TestTrue(TEXT("bind declared rays"), Inputs.Bind(Registry, Specs, Bindings, Error))) { AddError(Error); return false; }
	FUERLInputSampleStamp Stamp; Stamp.Sequence = 1; Stamp.SolverTimeSeconds = 0.02;
	for (int32 Slot = 0; Slot < 2; ++Slot)
	{
		TConstArrayView<float> Values;
		if (!TestTrue(TEXT("sample scene binding"), Inputs.Sample(Slot, Stamp, Values, Error))) { AddError(Error); return false; }
		if (!TestEqual(TEXT("two height rays and one clearance"), Values.Num(), 3)) { return false; }
		TestTrue(TEXT("plane height is -1 metre"), FMath::IsNearlyEqual(Values[0], -1.0f, 1.e-5f));
		TestTrue(TEXT("step height is -0.5 metre"), FMath::IsNearlyEqual(Values[1], -0.5f, 1.e-5f));
		TestTrue(TEXT("clearance is positive 1 metre"), FMath::IsNearlyEqual(Values[2], 1.0f, 1.e-5f));
	}
	return true;
}

#endif

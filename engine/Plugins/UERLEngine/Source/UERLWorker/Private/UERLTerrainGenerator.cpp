#include "UERLTerrainGenerator.h"

#include "UERLTerrainAtlas.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMemory.h"
#include "Logging/LogMacros.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "ProceduralMeshComponent.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUERLTerrainGenerator, Log, All);

namespace
{
	constexpr double CentimetresPerMetre = 100.0;
	UMaterialInterface* TerrainDisplayMaterial()
	{
		return LoadObject<UMaterialInterface>(
			nullptr,
			TEXT("/Engine/EngineMaterials/M_Grid.M_Grid"));
	}
	void AddBoxVisualInstances(
		UHierarchicalInstancedStaticMeshComponent& Visual,
		const TArray<FUERLTerrainBoxSpec>& Specs,
		int32 SlotId)
	{
		for (const FUERLTerrainBoxSpec& Spec : Specs)
		{
			if (SlotId != INDEX_NONE && Spec.SlotId != SlotId)
			{
				continue;
			}
			const FVector Location = Spec.CenterMeters * CentimetresPerMetre;
			const FVector Scale = Spec.ExtentMeters * 2.0;
			const FRotator Rotation(0.0, FMath::RadiansToDegrees(Spec.YawRadians), 0.0);
			Visual.AddInstance(FTransform(Rotation, Location, Scale), false);
		}
	}
	void AppendTerrainMesh(
		const FUERLTerrainMeshSpec& Source,
		FUERLTerrainMeshSpec& Destination)
	{
		check(Source.Normals.IsEmpty() || Source.Normals.Num() == Source.VerticesMeters.Num());
		const int32 BaseVertex = Destination.VerticesMeters.Num();
		if (Source.Normals.IsEmpty())
		{
			check(Destination.Normals.IsEmpty());
		}
		else
		{
			check(Destination.Normals.Num() == BaseVertex);
		}
		Destination.VerticesMeters.Append(Source.VerticesMeters);
		if (!Source.Normals.IsEmpty())
		{
			Destination.Normals.Append(Source.Normals);
		}
		for (const int32 Index : Source.Triangles)
		{
			Destination.Triangles.Add(BaseVertex + Index);
		}
	}
	void CreateCollisionSection(
		UProceduralMeshComponent& Component,
		const FUERLTerrainMeshSpec& Mesh,
		bool bVisible)
	{
		TArray<FVector> Vertices;
		Vertices.Reserve(Mesh.VerticesMeters.Num());
		for (const FVector& Vertex : Mesh.VerticesMeters)
		{
			Vertices.Add(Vertex * CentimetresPerMetre);
		}
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FLinearColor> Colors;
		TArray<FProcMeshTangent> Tangents;
		if (bVisible && Mesh.Normals.Num() == Mesh.VerticesMeters.Num())
		{
			Normals = Mesh.Normals;
			UVs.Reserve(Mesh.VerticesMeters.Num());
			Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), Mesh.VerticesMeters.Num());
			for (const FVector& Vertex : Mesh.VerticesMeters)
			{
				UVs.Add(FVector2D(Vertex.X, Vertex.Y));
			}
		}
		Component.CreateMeshSection_LinearColor(
			0, Vertices, Mesh.Triangles, Normals, UVs, Colors, Tangents, true);
	}
	void CompactTerrainPlans(TArray<FUERLTerrainTierPlan>& Plans)
	{
		for (FUERLTerrainTierPlan& Plan : Plans)
		{
			Plan.Boxes.Reset();
			Plan.Meshes.Reset();
			Plan.CollisionMeshes.Reset();
		}
	}
}

bool FUERLTerrainGenerator::BuildPlan(
	const FUERLTerrainConfig& Config,
	const TArray<FVector>& SlotOrigins,
	TArray<FUERLTerrainTierPlan>& OutPlans,
	FString& OutError)
{
	return FUERLTerrainAtlas::BuildPlan(Config, SlotOrigins, OutPlans, OutError);
}

bool FUERLTerrainGenerator::BuildSharedPlan(
	const FUERLTerrainConfig& Config,
	const TArray<FVector>& SlotOrigins,
	TArray<FUERLTerrainTierPlan>& OutPlans,
	FString& OutError)
{
	return FUERLTerrainAtlas::BuildSharedPlan(Config, SlotOrigins, OutPlans, OutError);
}

bool FUERLTerrainGenerator::Generate(
	UWorld& World,
	const FUERLTerrainConfig& Config,
	TArray<FUERLSlotContext>& SlotContexts,
	FString& OutError)
{
	const double TerrainStart = FPlatformTime::Seconds();
	Destroy();
	bPhysicsCollision = Config.bPhysicsCollision;
	TArray<FVector> SlotOrigins;
	SlotOrigins.Reserve(SlotContexts.Num());
	for (const FUERLSlotContext& Slot : SlotContexts)
	{
		SlotOrigins.Add(Slot.Origin);
	}
	TArray<FUERLTerrainTierPlan> NewPlans;
	const double BuildStart = FPlatformTime::Seconds();
	if (!BuildPlan(Config, SlotOrigins, NewPlans, OutError))
	{
		Destroy();
		return false;
	}
	const double BuildSeconds = FPlatformTime::Seconds() - BuildStart;
	const double SpawnStart = FPlatformTime::Seconds();
	if (!SpawnPlans(World, NewPlans, SlotContexts, OutError))
	{
		Destroy();
		return false;
	}
	const double SpawnSeconds = FPlatformTime::Seconds() - SpawnStart;
	const FVector2D TerrainHalfExtent(
		(Config.CellSize[0] + 2.0 * Config.BorderWidth) * CentimetresPerMetre * 0.5,
		(Config.CellSize[1] + 2.0 * Config.BorderWidth) * CentimetresPerMetre * 0.5);
	for (FUERLSlotContext& Slot : SlotContexts)
	{
		Slot.TerrainHalfExtent = TerrainHalfExtent;
		Slot.bTerrainBoundsValid = true;
	}
	CompactTerrainPlans(NewPlans);
	Plans = MoveTemp(NewPlans);
	const double TerrainTotalSeconds = FPlatformTime::Seconds() - TerrainStart;
	UE_LOG(LogUERLTerrainGenerator, Display,
		TEXT("[TerrainPerf] route=isolated build_s=%.6f spawn_total_s=%.6f terrain_total_s=%.6f slots=%d levels=%d physics_collision=%d"),
		BuildSeconds, SpawnSeconds, TerrainTotalSeconds, SlotContexts.Num(), Config.NumLevels,
		bPhysicsCollision ? 1 : 0);
	return true;
}

bool FUERLTerrainGenerator::GenerateShared(
	UWorld& World,
	const FUERLTerrainConfig& Config,
	TArray<FUERLSlotContext>& SlotContexts,
	FString& OutError)
{
	const double TerrainStart = FPlatformTime::Seconds();
	Destroy();
	bPhysicsCollision = Config.bPhysicsCollision;
	TArray<FVector> SlotOrigins;
	SlotOrigins.Reserve(SlotContexts.Num());
	for (const FUERLSlotContext& Slot : SlotContexts)
	{
		SlotOrigins.Add(Slot.Origin);
	}
	TArray<FUERLTerrainTierPlan> NewPlans;
	const double BuildStart = FPlatformTime::Seconds();
	if (!BuildSharedPlan(Config, SlotOrigins, NewPlans, OutError))
	{
		Destroy();
		return false;
	}
	const double BuildSeconds = FPlatformTime::Seconds() - BuildStart;
	const double SpawnStart = FPlatformTime::Seconds();
	if (!SpawnSharedPlans(World, NewPlans, SlotContexts, OutError))
	{
		Destroy();
		return false;
	}
	const double SpawnSeconds = FPlatformTime::Seconds() - SpawnStart;
	const FVector2D TerrainHalfExtent(
		(Config.CellSize[0] + 2.0 * Config.BorderWidth) * CentimetresPerMetre * 0.5,
		(Config.CellSize[1] + 2.0 * Config.BorderWidth) * CentimetresPerMetre * 0.5);
	for (FUERLSlotContext& Slot : SlotContexts)
	{
		Slot.TerrainHalfExtent = TerrainHalfExtent;
		// SharedWorld keeps all Slot origins inside the active atlas tile.  The
		// tile center is the atlas origin, not the individual Slot origin; retain
		// that offset when a reset later selects another pre-generated level.
		Slot.TerrainBoundsOriginOffset = -Slot.Origin;
		Slot.bTerrainBoundsValid = true;
	}
	for (FUERLSlotContext& Slot : SlotContexts)
	{
		Slot.TerrainQueryActors = TerrainActors;
	}
	CompactTerrainPlans(NewPlans);
	Plans = MoveTemp(NewPlans);
	const double TerrainTotalSeconds = FPlatformTime::Seconds() - TerrainStart;
	UE_LOG(LogUERLTerrainGenerator, Display,
		TEXT("[TerrainPerf] route=shared build_s=%.6f spawn_total_s=%.6f terrain_total_s=%.6f slots=%d levels=%d physics_collision=%d"),
		BuildSeconds, SpawnSeconds, TerrainTotalSeconds, SlotContexts.Num(), Config.NumLevels,
		bPhysicsCollision ? 1 : 0);
	return true;
}

bool FUERLTerrainGenerator::SpawnPlans(
	UWorld& World,
	const TArray<FUERLTerrainTierPlan>& InPlans,
	TArray<FUERLSlotContext>& SlotContexts,
	FString& OutError)
{
	// Keep one owner per isolated Slot and attach every pre-generated tier to it.
	// Visual instances are added in tier order; each collision layer is submitted
	// only after all of that Slot's tiers have been merged.
	for (int32 SlotId = 0; SlotId < SlotContexts.Num(); ++SlotId)
	{
		AActor* Owner = World.SpawnActor<AActor>(
			AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
		if (!Owner)
		{
			OutError = FString::Printf(
				TEXT("failed to spawn isolated terrain atlas actor for Slot %d"), SlotId);
			return false;
		}
		TerrainActors.Add(Owner);
		SlotContexts[SlotId].EnvironmentActors.Add(Owner);

		USceneComponent* AtlasRoot = NewObject<USceneComponent>(Owner, TEXT("TerrainAtlasRoot"));
		Owner->SetRootComponent(AtlasRoot);
		AtlasRoot->SetMobility(EComponentMobility::Static);
		AtlasRoot->RegisterComponent();

		UHierarchicalInstancedStaticMeshComponent* Visual = nullptr;
		UStaticMesh* Cube = nullptr;
		UMaterialInterface* DisplayMaterial = nullptr;
		const double VisualStart = FPlatformTime::Seconds();
		for (const FUERLTerrainTierPlan& Plan : InPlans)
		{
			bool bHasVisualBoxes = false;
			for (const FUERLTerrainBoxSpec& Spec : Plan.Boxes)
			{
				if (Spec.SlotId == SlotId)
				{
					bHasVisualBoxes = true;
					break;
				}
			}
			if (!bHasVisualBoxes)
			{
				continue;
			}
			if (!Cube)
			{
				Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
				if (!Cube)
				{
					OutError = TEXT("terrain visual layer cannot load the Engine cube mesh");
					return false;
				}
			}
			if (!Visual)
			{
				Visual = NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner, TEXT("TerrainVisualInstances"));
				Visual->SetupAttachment(AtlasRoot);
				Visual->SetMobility(EComponentMobility::Static);
				Visual->SetStaticMesh(Cube);
				Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Visual->SetGenerateOverlapEvents(false);
				Visual->SetCanEverAffectNavigation(false);
				DisplayMaterial = TerrainDisplayMaterial();
				Visual->SetMaterial(0, DisplayMaterial);
				Visual->RegisterComponent();
			}
			AddBoxVisualInstances(*Visual, Plan.Boxes, SlotId);
		}
		const double VisualSeconds = FPlatformTime::Seconds() - VisualStart;

		FUERLTerrainMeshSpec CollisionMesh;
		FUERLTerrainMeshSpec HeightfieldMesh;
		const double CollisionPrepareStart = FPlatformTime::Seconds();
		int32 CollisionVertexCount = 0;
		int32 CollisionIndexCount = 0;
		int32 HeightfieldVertexCount = 0;
		int32 HeightfieldIndexCount = 0;
		int32 HeightfieldNormalCount = 0;
		for (const FUERLTerrainTierPlan& Plan : InPlans)
		{
			for (const FUERLTerrainMeshSpec& Mesh : Plan.CollisionMeshes)
			{
				if (Mesh.SlotId == SlotId && !Mesh.Triangles.IsEmpty())
				{
					CollisionVertexCount += Mesh.VerticesMeters.Num();
					CollisionIndexCount += Mesh.Triangles.Num();
				}
			}
			for (const FUERLTerrainMeshSpec& Mesh : Plan.Meshes)
			{
				if (Mesh.SlotId == SlotId && !Mesh.Triangles.IsEmpty())
				{
					HeightfieldVertexCount += Mesh.VerticesMeters.Num();
					HeightfieldIndexCount += Mesh.Triangles.Num();
					HeightfieldNormalCount += Mesh.Normals.Num();
				}
			}
		}
		CollisionMesh.VerticesMeters.Reserve(CollisionVertexCount);
		CollisionMesh.Triangles.Reserve(CollisionIndexCount);
		HeightfieldMesh.VerticesMeters.Reserve(HeightfieldVertexCount);
		HeightfieldMesh.Triangles.Reserve(HeightfieldIndexCount);
		if (HeightfieldNormalCount > 0)
		{
			HeightfieldMesh.Normals.Reserve(HeightfieldNormalCount);
		}
		for (const FUERLTerrainTierPlan& Plan : InPlans)
		{
			for (const FUERLTerrainMeshSpec& Mesh : Plan.CollisionMeshes)
			{
				if (Mesh.SlotId == SlotId && !Mesh.Triangles.IsEmpty())
				{
					AppendTerrainMesh(Mesh, CollisionMesh);
				}
			}
			for (const FUERLTerrainMeshSpec& Mesh : Plan.Meshes)
			{
				if (Mesh.SlotId == SlotId && !Mesh.Triangles.IsEmpty())
				{
					AppendTerrainMesh(Mesh, HeightfieldMesh);
				}
			}
		}
		const double CollisionPrepareSeconds = FPlatformTime::Seconds() - CollisionPrepareStart;
		const double CollisionStart = FPlatformTime::Seconds();
		int32 CollisionBodyCount = 0;
		int32 HeightfieldBodyCount = 0;
		int32 CollisionSectionCount = 0;
		int32 HeightfieldSectionCount = 0;
		if (!CollisionMesh.Triangles.IsEmpty())
		{
			UProceduralMeshComponent* Collision = NewObject<UProceduralMeshComponent>(
				Owner, TEXT("TerrainCollision"));
			Collision->SetupAttachment(AtlasRoot);
			Collision->SetMobility(EComponentMobility::Static);
			SlotContexts[SlotId].CollisionProfile.ApplySlotEnvironment(*Collision);
			Collision->SetCollisionEnabled(
				bPhysicsCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly);
			Collision->SetVisibility(false);
			Collision->SetHiddenInGame(true);
			Collision->RegisterComponent();
			CreateCollisionSection(*Collision, CollisionMesh, false);
			SlotContexts[SlotId].GroundComponents.Add(Collision);
			++CollisionBodyCount;
			CollisionSectionCount = Collision->GetNumSections();
		}
		if (!HeightfieldMesh.Triangles.IsEmpty())
		{
			UProceduralMeshComponent* Heightfield = NewObject<UProceduralMeshComponent>(
				Owner, TEXT("TerrainHeightfield"));
			Heightfield->SetupAttachment(AtlasRoot);
			Heightfield->SetMobility(EComponentMobility::Static);
			Heightfield->SetMaterial(0, DisplayMaterial ? DisplayMaterial : TerrainDisplayMaterial());
			SlotContexts[SlotId].CollisionProfile.ApplySlotEnvironment(*Heightfield);
			Heightfield->SetCollisionEnabled(
				bPhysicsCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly);
			Heightfield->RegisterComponent();
			CreateCollisionSection(*Heightfield, HeightfieldMesh, true);
			SlotContexts[SlotId].GroundComponents.Add(Heightfield);
			++HeightfieldBodyCount;
			HeightfieldSectionCount = Heightfield->GetNumSections();
		}
		const double CollisionSeconds = FPlatformTime::Seconds() - CollisionStart;
		const int32 FinalBodyCount = CollisionBodyCount + HeightfieldBodyCount;
		const int32 FinalVertices = CollisionMesh.VerticesMeters.Num() + HeightfieldMesh.VerticesMeters.Num();
		const int32 FinalIndices = CollisionMesh.Triangles.Num() + HeightfieldMesh.Triangles.Num();
		const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
		UE_LOG(LogUERLTerrainGenerator, Display,
			TEXT("[TerrainPerf] route=isolated slot=%d visual_s=%.6f collision_prepare_s=%.6f collision_s=%.6f visual_instances=%d final_body_count=%d final_section_count=%d final_vertices=%d final_indices=%d final_triangles=%d collision_bodies=%d collision_sections=%d collision_vertices=%d collision_indices=%d collision_triangles=%d heightfield_bodies=%d heightfield_sections=%d heightfield_vertices=%d heightfield_indices=%d heightfield_triangles=%d peak_working_set_mb=%.3f"),
			SlotId, VisualSeconds, CollisionPrepareSeconds, CollisionSeconds,
			Visual ? Visual->GetInstanceCount() : 0,
			FinalBodyCount, CollisionSectionCount + HeightfieldSectionCount,
			FinalVertices, FinalIndices, FinalIndices / 3,
			CollisionBodyCount, CollisionSectionCount,
			CollisionMesh.VerticesMeters.Num(), CollisionMesh.Triangles.Num(), CollisionMesh.Triangles.Num() / 3,
			HeightfieldBodyCount, HeightfieldSectionCount,
			HeightfieldMesh.VerticesMeters.Num(), HeightfieldMesh.Triangles.Num(), HeightfieldMesh.Triangles.Num() / 3,
			static_cast<double>(MemoryStats.PeakUsedPhysical) / (1024.0 * 1024.0));
	}
	return true;
}

bool FUERLTerrainGenerator::SpawnSharedPlans(
	UWorld& World,
	const TArray<FUERLTerrainTierPlan>& InPlans,
	TArray<FUERLSlotContext>& SlotContexts,
	FString& OutError)
{
	// SharedWorld owns one generated atlas actor.  Visual instances remain in
	// tier order, while each collision layer is submitted as one final section.
	const double SpawnStart = FPlatformTime::Seconds();
	AActor* Owner = World.SpawnActor<AActor>(
		AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	if (!Owner)
	{
		OutError = TEXT("failed to spawn shared terrain atlas actor");
		return false;
	}
	TerrainActors.Add(Owner);

	USceneComponent* AtlasRoot = NewObject<USceneComponent>(Owner, TEXT("SharedTerrainAtlasRoot"));
	Owner->SetRootComponent(AtlasRoot);
	AtlasRoot->SetMobility(EComponentMobility::Static);
	AtlasRoot->RegisterComponent();

	UHierarchicalInstancedStaticMeshComponent* Visual = nullptr;
	UStaticMesh* Cube = nullptr;
	UMaterialInterface* DisplayMaterial = nullptr;
	const double VisualStart = FPlatformTime::Seconds();
	for (const FUERLTerrainTierPlan& Plan : InPlans)
	{
		if (Plan.Boxes.IsEmpty())
		{
			continue;
		}
		if (!Cube)
		{
			Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (!Cube)
			{
				OutError = TEXT("terrain visual layer cannot load the Engine cube mesh");
				return false;
			}
		}
		if (!Visual)
		{
			Visual = NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner, TEXT("TerrainVisualInstances"));
			Visual->SetupAttachment(AtlasRoot);
			Visual->SetMobility(EComponentMobility::Static);
			Visual->SetStaticMesh(Cube);
			Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Visual->SetGenerateOverlapEvents(false);
			Visual->SetCanEverAffectNavigation(false);
			DisplayMaterial = TerrainDisplayMaterial();
			Visual->SetMaterial(0, DisplayMaterial);
			Visual->RegisterComponent();
		}
		AddBoxVisualInstances(*Visual, Plan.Boxes, INDEX_NONE);
	}
	const double VisualSeconds = FPlatformTime::Seconds() - VisualStart;

	FUERLTerrainMeshSpec CollisionMesh;
	FUERLTerrainMeshSpec HeightfieldMesh;
	const double CollisionPrepareStart = FPlatformTime::Seconds();
	int32 CollisionVertexCount = 0;
	int32 CollisionIndexCount = 0;
	int32 HeightfieldVertexCount = 0;
	int32 HeightfieldIndexCount = 0;
	int32 HeightfieldNormalCount = 0;
	for (const FUERLTerrainTierPlan& Plan : InPlans)
	{
		for (const FUERLTerrainMeshSpec& Mesh : Plan.CollisionMeshes)
		{
			if (Mesh.SlotId == INDEX_NONE && !Mesh.Triangles.IsEmpty())
			{
				CollisionVertexCount += Mesh.VerticesMeters.Num();
				CollisionIndexCount += Mesh.Triangles.Num();
			}
		}
		for (const FUERLTerrainMeshSpec& Mesh : Plan.Meshes)
		{
			if (Mesh.SlotId == INDEX_NONE && !Mesh.Triangles.IsEmpty())
			{
				HeightfieldVertexCount += Mesh.VerticesMeters.Num();
				HeightfieldIndexCount += Mesh.Triangles.Num();
				HeightfieldNormalCount += Mesh.Normals.Num();
			}
		}
	}
	CollisionMesh.VerticesMeters.Reserve(CollisionVertexCount);
	CollisionMesh.Triangles.Reserve(CollisionIndexCount);
	HeightfieldMesh.VerticesMeters.Reserve(HeightfieldVertexCount);
	HeightfieldMesh.Triangles.Reserve(HeightfieldIndexCount);
	if (HeightfieldNormalCount > 0)
	{
		HeightfieldMesh.Normals.Reserve(HeightfieldNormalCount);
	}
	for (const FUERLTerrainTierPlan& Plan : InPlans)
	{
		for (const FUERLTerrainMeshSpec& Mesh : Plan.CollisionMeshes)
		{
			if (Mesh.SlotId == INDEX_NONE && !Mesh.Triangles.IsEmpty())
			{
				AppendTerrainMesh(Mesh, CollisionMesh);
			}
		}
		for (const FUERLTerrainMeshSpec& Mesh : Plan.Meshes)
		{
			if (Mesh.SlotId == INDEX_NONE && !Mesh.Triangles.IsEmpty())
			{
				AppendTerrainMesh(Mesh, HeightfieldMesh);
			}
		}
	}
	const double CollisionPrepareSeconds = FPlatformTime::Seconds() - CollisionPrepareStart;
	const double CollisionStart = FPlatformTime::Seconds();
	int32 CollisionBodyCount = 0;
	int32 HeightfieldBodyCount = 0;
	int32 CollisionSectionCount = 0;
	int32 HeightfieldSectionCount = 0;
	UProceduralMeshComponent* Collision = nullptr;
	UProceduralMeshComponent* Heightfield = nullptr;
	if (!CollisionMesh.Triangles.IsEmpty())
	{
		Collision = NewObject<UProceduralMeshComponent>(
			Owner, TEXT("TerrainCollision"));
		Collision->SetupAttachment(AtlasRoot);
		Collision->SetMobility(EComponentMobility::Static);
		Collision->SetCollisionObjectType(ECC_WorldStatic);
		Collision->SetCollisionResponseToAllChannels(ECR_Block);
		Collision->SetCollisionEnabled(
			bPhysicsCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly);
		Collision->SetVisibility(false);
		Collision->SetHiddenInGame(true);
		Collision->RegisterComponent();
		CreateCollisionSection(*Collision, CollisionMesh, false);
		++CollisionBodyCount;
		CollisionSectionCount = Collision->GetNumSections();
	}
	if (!HeightfieldMesh.Triangles.IsEmpty())
	{
		Heightfield = NewObject<UProceduralMeshComponent>(
			Owner, TEXT("TerrainHeightfield"));
		Heightfield->SetupAttachment(AtlasRoot);
		Heightfield->SetMobility(EComponentMobility::Static);
		Heightfield->SetMaterial(0, DisplayMaterial ? DisplayMaterial : TerrainDisplayMaterial());
		Heightfield->SetCollisionObjectType(ECC_WorldStatic);
		Heightfield->SetCollisionResponseToAllChannels(ECR_Block);
		Heightfield->SetCollisionEnabled(
			bPhysicsCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::QueryOnly);
		Heightfield->RegisterComponent();
		CreateCollisionSection(*Heightfield, HeightfieldMesh, true);
		++HeightfieldBodyCount;
		HeightfieldSectionCount = Heightfield->GetNumSections();
	}
	for (FUERLSlotContext& Slot : SlotContexts)
	{
		if (Collision) { Slot.GroundComponents.Add(Collision); }
		if (Heightfield) { Slot.GroundComponents.Add(Heightfield); }
	}
	const double CollisionSeconds = FPlatformTime::Seconds() - CollisionStart;
	const int32 FinalBodyCount = CollisionBodyCount + HeightfieldBodyCount;
	const int32 FinalVertices = CollisionMesh.VerticesMeters.Num() + HeightfieldMesh.VerticesMeters.Num();
	const int32 FinalIndices = CollisionMesh.Triangles.Num() + HeightfieldMesh.Triangles.Num();
	const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
	UE_LOG(LogUERLTerrainGenerator, Display,
		TEXT("[TerrainPerf] route=shared visual_s=%.6f collision_prepare_s=%.6f collision_s=%.6f spawn_s=%.6f visual_instances=%d final_body_count=%d final_section_count=%d final_vertices=%d final_indices=%d final_triangles=%d collision_bodies=%d collision_sections=%d collision_vertices=%d collision_indices=%d collision_triangles=%d heightfield_bodies=%d heightfield_sections=%d heightfield_vertices=%d heightfield_indices=%d heightfield_triangles=%d peak_working_set_mb=%.3f"),
		VisualSeconds, CollisionPrepareSeconds, CollisionSeconds, FPlatformTime::Seconds() - SpawnStart,
		Visual ? Visual->GetInstanceCount() : 0,
		FinalBodyCount, CollisionSectionCount + HeightfieldSectionCount,
		FinalVertices, FinalIndices, FinalIndices / 3,
		CollisionBodyCount, CollisionSectionCount,
		CollisionMesh.VerticesMeters.Num(), CollisionMesh.Triangles.Num(), CollisionMesh.Triangles.Num() / 3,
		HeightfieldBodyCount, HeightfieldSectionCount,
		HeightfieldMesh.VerticesMeters.Num(), HeightfieldMesh.Triangles.Num(), HeightfieldMesh.Triangles.Num() / 3,
		static_cast<double>(MemoryStats.PeakUsedPhysical) / (1024.0 * 1024.0));
	return true;
}

void FUERLTerrainGenerator::Destroy()
{
	for (const TWeakObjectPtr<AActor>& Actor : TerrainActors)
	{
		if (Actor.IsValid())
		{
			Actor->Destroy();
		}
	}
	TerrainActors.Reset();
	Plans.Reset();
}

bool FUERLTerrainGenerator::TryGetSample(
	int32 Level,
	int32 SlotId,
	FUERLTerrainSpawnSample& OutSample) const
{
	if (!Plans.IsValidIndex(Level) || !Plans[Level].Samples.IsValidIndex(SlotId))
	{
		return false;
	}
	OutSample = Plans[Level].Samples[SlotId];
	return true;
}

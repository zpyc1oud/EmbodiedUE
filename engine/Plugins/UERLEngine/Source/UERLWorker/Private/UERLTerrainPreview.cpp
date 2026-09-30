#include "CoreMinimal.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "UObject/UObjectGlobals.h"

#include "UERLInterfaceTypes.h"
#include "UERLTerrainGenerator.h"
#include "UERLWorkerLog.h"

// Static, editor-facing preview of the terrain catalogue.
//
// This is a visualisation-only path: it rebuilds the four catalogue terrains
// (plane, heightfield, boxes, hand-placed obstacles) through the same
// deterministic FUERLTerrainGenerator::BuildPlan the Worker uses, then spawns
// visible meshes with Unreal's default grid material so a human can inspect them
// in an editor viewport. It never runs a training session and is not wired into
// the Bridge.

namespace
{
	constexpr double TerrainPreviewCentimetresPerMetre = 100.0;

	// Build the four-tier catalogue config the same way the UE unit test does.
	FUERLTerrainConfig BuildCatalogConfig()
	{
		FUERLTerrainConfig Config;
		Config.NumLevels = 4;
		Config.CellSize[0] = 8.0;
		Config.CellSize[1] = 8.0;
		Config.BorderWidth = 0.5;
		Config.Tiers.Reserve(Config.NumLevels);

		FUERLTerrainTierConfig& Plane = Config.Tiers.AddDefaulted_GetRef();
		Plane.Level = 0;
		Plane.Primitive = EUERLTerrainPrimitive::Plane;
		Plane.Seed = 1;
		Plane.Params = MakeShared<FJsonObject>();

		FUERLTerrainTierConfig& Heightfield = Config.Tiers.AddDefaulted_GetRef();
		Heightfield.Level = 1;
		Heightfield.Primitive = EUERLTerrainPrimitive::Heightfield;
		Heightfield.Seed = 2;
		Heightfield.PlatformWidth = 1.5;
		Heightfield.Params = MakeShared<FJsonObject>();
		Heightfield.Params->SetArrayField(TEXT("noise_range"), {
			MakeShared<FJsonValueNumber>(0.05), MakeShared<FJsonValueNumber>(0.4) });
		Heightfield.Params->SetNumberField(TEXT("noise_step"), 0.02);
		Heightfield.Params->SetNumberField(TEXT("horizontal_scale"), 0.25);
		Heightfield.Params->SetNumberField(TEXT("vertical_scale"), 0.02);
		// downsampled_scale > horizontal_scale => coarse random samples are
		// interpolated into smooth, wide rolling hills rather than sharp noise.
		Heightfield.Params->SetNumberField(TEXT("downsampled_scale"), 1.5);

		FUERLTerrainTierConfig& Boxes = Config.Tiers.AddDefaulted_GetRef();
		Boxes.Level = 2;
		Boxes.Primitive = EUERLTerrainPrimitive::Boxes;
		Boxes.Seed = 3;
		Boxes.PlatformWidth = 1.5;
		Boxes.Params = MakeShared<FJsonObject>();
		Boxes.Params->SetNumberField(TEXT("grid_width"), 0.6);
		Boxes.Params->SetArrayField(TEXT("grid_height_range"), {
			MakeShared<FJsonValueNumber>(0.05), MakeShared<FJsonValueNumber>(0.35) });
		Boxes.Params->SetBoolField(TEXT("holes"), false);
		// Isaac Lab-style independent random grid samples are rasterized into
		// discrete piecewise-constant cuboids by FUERLTerrainGenerator.
		Boxes.Params->SetStringField(TEXT("generator"), TEXT("random_grid"));
		Boxes.Params->SetNumberField(TEXT("difficulty"), 1.0);

		FUERLTerrainTierConfig& Explicit = Config.Tiers.AddDefaulted_GetRef();
		Explicit.Level = 3;
		Explicit.Primitive = EUERLTerrainPrimitive::Boxes;
		Explicit.Seed = 4;
		Explicit.PlatformWidth = 1.5;
		Explicit.Params = MakeShared<FJsonObject>();
		{
			TArray<TSharedPtr<FJsonValue>> Obstacles;
			auto AddObstacle = [&Obstacles](double X, double Y, double HalfZ)
			{
				TSharedPtr<FJsonObject> Obstacle = MakeShared<FJsonObject>();
				Obstacle->SetArrayField(TEXT("pose"), {
					MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y),
					MakeShared<FJsonValueNumber>(HalfZ), MakeShared<FJsonValueNumber>(0.0) });
				Obstacle->SetArrayField(TEXT("extent"), {
					MakeShared<FJsonValueNumber>(0.3), MakeShared<FJsonValueNumber>(0.3),
					MakeShared<FJsonValueNumber>(HalfZ) });
				Obstacles.Add(MakeShared<FJsonValueObject>(Obstacle));
			};
			AddObstacle(1.5, 0.0, 0.25);
			AddObstacle(-1.5, 1.0, 0.4);
			AddObstacle(0.0, -1.8, 0.15);
			Explicit.Params->SetArrayField(TEXT("explicit"), Obstacles);
		}

		return Config;
	}

	// Unreal's default checkered grid material. It is untinted and its world-space
	// grid makes slopes, steps and undulation easy to read by eye.
	UMaterialInterface* LoadDefaultMaterial()
	{
		return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/M_Grid.M_Grid"));
	}

	void SpawnCatalogPreview(UWorld& World)
	{
		const FUERLTerrainConfig Config = BuildCatalogConfig();

		// Build one slot; BuildPlan lays the four tiers side by side along X.
		// Passing one origin keeps the preview at one copy of each catalog terrain.
		const double TierSpacingMetres = Config.CellSize[0] + 2.0 * Config.BorderWidth;
		const TArray<FVector> SlotOrigins = { FVector::ZeroVector };

		TArray<FUERLTerrainTierPlan> Plans;
		FString Error;
		if (!FUERLTerrainGenerator::BuildPlan(Config, SlotOrigins, Plans, Error))
		{
			UE_LOG(LogUERLWorker, Error, TEXT("[terrain-preview] BuildPlan failed: %s"), *Error);
			return;
		}

		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube)
		{
			UE_LOG(LogUERLWorker, Error, TEXT("[terrain-preview] cannot load Engine Cube mesh"));
			return;
		}
		UMaterialInterface* DefaultMaterial = LoadDefaultMaterial();

		int32 SpawnedActors = 0;
		for (int32 TierIndex = 0; TierIndex < Plans.Num(); ++TierIndex)
		{
			const FUERLTerrainTierPlan& Plan = Plans[TierIndex];

			AActor* Owner = World.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
			if (!Owner)
			{
				UE_LOG(LogUERLWorker, Error, TEXT("[terrain-preview] failed to spawn actor for tier %d"), Plan.Level);
				continue;
			}
#if WITH_EDITOR
			Owner->SetActorLabel(FString::Printf(TEXT("TerrainPreview_L%d"), Plan.Level));
#endif
			++SpawnedActors;

			if (Plan.Boxes.Num() > 0)
			{
				UInstancedStaticMeshComponent* Boxes = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("PreviewBoxes"));
				Owner->SetRootComponent(Boxes);
				Boxes->SetMobility(EComponentMobility::Static);
				Boxes->SetStaticMesh(Cube);
				// The preview mirrors the runtime split: instance geometry is visual
				// only, while training collision is owned by the merged patch mesh.
				Boxes->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Boxes->SetGenerateOverlapEvents(false);
				Boxes->SetCanEverAffectNavigation(false);
				Boxes->SetMaterial(0, DefaultMaterial);
				Boxes->RegisterComponent();
				for (const FUERLTerrainBoxSpec& Spec : Plan.Boxes)
				{
					const FVector Location = Spec.CenterMeters * TerrainPreviewCentimetresPerMetre;
					const FVector Scale = Spec.ExtentMeters * 2.0;
					const FRotator Rotation(0.0, FMath::RadiansToDegrees(Spec.YawRadians), 0.0);
					Boxes->AddInstance(FTransform(Rotation, Location, Scale), false);
				}
			}

			if (Plan.Meshes.Num() > 0)
			{
				UProceduralMeshComponent* Heightfield = NewObject<UProceduralMeshComponent>(Owner, TEXT("PreviewHeightfield"));
				Owner->SetRootComponent(Heightfield);
				Heightfield->SetMobility(EComponentMobility::Static);
				Heightfield->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Heightfield->SetMaterial(0, DefaultMaterial);
				Heightfield->RegisterComponent();
				for (int32 Section = 0; Section < Plan.Meshes.Num(); ++Section)
				{
					const FUERLTerrainMeshSpec& Mesh = Plan.Meshes[Section];
					TArray<FVector> Vertices;
					Vertices.Reserve(Mesh.VerticesMeters.Num());
					TArray<FVector2D> UVs;
					UVs.Reserve(Mesh.VerticesMeters.Num());
					for (const FVector& Vertex : Mesh.VerticesMeters)
					{
						Vertices.Add(Vertex * TerrainPreviewCentimetresPerMetre);
						// One UV tile per metre so the default grid material reads as scale.
						UVs.Add(FVector2D(Vertex.X, Vertex.Y));
					}
					TArray<FLinearColor> Colors;
					TArray<FProcMeshTangent> Tangents;
					Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), Vertices.Num());
					TArray<FVector> Normals = Mesh.Normals;
					Heightfield->CreateMeshSection_LinearColor(
						Section, Vertices, Mesh.Triangles, Normals, UVs, Colors, Tangents, true);
				}
			}
		}

		// Lighting is owned by FUERLViewportObserver in viewport mode. The preview
		// command only adds terrain so it never creates a second custom sky rig.

		// Point the player camera at the middle of the four side-by-side terrains.
		const double CentreX = (Config.NumLevels - 1) * 0.5 * TierSpacingMetres * TerrainPreviewCentimetresPerMetre;
		const FVector CentreOfTerrains(CentreX, 0.0, 0.0);
		const double SpanCentimetres = Config.NumLevels * TierSpacingMetres * TerrainPreviewCentimetresPerMetre;
		const FVector CameraLocation = CentreOfTerrains + FVector(-SpanCentimetres * 0.9, 0.0, SpanCentimetres * 0.7);
		if (APlayerController* Controller = UGameplayStatics::GetPlayerController(&World, 0))
		{
			const FRotator LookAt = (CentreOfTerrains - CameraLocation).Rotation();
			Controller->SetControlRotation(LookAt);
			if (APawn* Pawn = Controller->GetPawn())
			{
				Pawn->SetActorLocationAndRotation(CameraLocation, LookAt);
			}
			else
			{
				Controller->SetInitialLocationAndRotation(CameraLocation, LookAt);
			}
		}

		UE_LOG(LogUERLWorker, Log,
			TEXT("[terrain-preview] spawned %d terrain tiers along +Y (L0 plane, L1 heightfield, L2 boxes, L3 hand-placed obstacles)"),
			SpawnedActors);
	}

	UWorld* ResolvePreviewWorld(UWorld* ContextWorld)
	{
		if (ContextWorld && (ContextWorld->WorldType == EWorldType::Editor
			|| ContextWorld->WorldType == EWorldType::PIE
			|| ContextWorld->WorldType == EWorldType::Game))
		{
			return ContextWorld;
		}
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.World() && (Context.WorldType == EWorldType::Editor
					|| Context.WorldType == EWorldType::PIE
					|| Context.WorldType == EWorldType::Game))
				{
					return Context.World();
				}
			}
		}
		return nullptr;
	}

	FAutoConsoleCommandWithWorld GTerrainPreviewCommand(
		TEXT("UERL.Terrain.Preview"),
		TEXT("Spawn the four catalogue terrains (plane/heightfield/boxes/obstacles) into the current world for visual inspection."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			UWorld* Target = ResolvePreviewWorld(World);
			if (!Target)
			{
				UE_LOG(LogUERLWorker, Error, TEXT("[terrain-preview] no editor/PIE/game world available"));
				return;
			}
			SpawnCatalogPreview(*Target);
		}));
}

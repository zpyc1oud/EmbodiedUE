#include "UERLEgyptChaseGameMode.h"

#include "CollisionQueryParams.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "UERLPolicyArtifactAsset.h"
#include "UERLPolicyComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogUERLEgyptChase, Log, All);

AUERLEgyptChaseGameMode::AUERLEgyptChaseGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AUERLEgyptChaseGameMode::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	bEgyptMap = World && World->GetMapName().Contains(TEXT("Stylized_Egypt"));
}

void AUERLEgyptChaseGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bEgyptMap)
	{
		return;
	}
	if (!PolicyComponent)
	{
		TrySpawnRobot();
		return;
	}

	FTransform RobotTransform;
	if (!PolicyComponent->GetRobotTransform(RobotTransform))
	{
		return;
	}
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!Player)
	{
		return;
	}
	FVector ToPlayer = Player->GetActorLocation() - RobotTransform.GetLocation();
	ToPlayer.Z = 0.0;
	const float DistanceMeters = ToPlayer.Size() / 100.0f;
	TArray<float> Command = {0.0f, 0.0f, 0.0f};
	if (DistanceMeters > 1.25f)
	{
		ToPlayer.Normalize();
		FVector Forward = RobotTransform.GetRotation().GetForwardVector();
		Forward.Z = 0.0;
		if (!Forward.Normalize())
		{
			Forward = FVector::ForwardVector;
		}
		const float YawError = FMath::Atan2(
			FVector::CrossProduct(Forward, ToPlayer).Z,
			FVector::DotProduct(Forward, ToPlayer));
		Command[0] = 0.45f;
		Command[2] = FMath::Clamp(YawError * 0.5f, -1.0f, 1.0f);
	}
	PolicyComponent->SetCommand(TEXT("velocity"), Command);
}

void AUERLEgyptChaseGameMode::TrySpawnRobot()
{
	UWorld* World = GetWorld();
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!World || !Player)
	{
		return;
	}

	UUERLPolicyArtifactAsset* Artifact = LoadObject<UUERLPolicyArtifactAsset>(
		nullptr,
		TEXT("/Game/UERLEngine/Policies/PhantomXContinuousTerrain.PhantomXContinuousTerrain"));
	if (!Artifact)
	{
		UE_LOG(LogUERLEgyptChase, Error, TEXT("Egypt chase could not load PhantomXContinuousTerrain"));
		bEgyptMap = false;
		return;
	}

	const FVector PlayerLocation = Player->GetActorLocation();
	FVector Probe = PlayerLocation + Player->GetActorForwardVector() * 300.0;
	Probe.Z = PlayerLocation.Z;
	FHitResult GroundHit;
	FCollisionQueryParams QueryParams(FCollisionQueryParams::DefaultQueryParam);
	QueryParams.bTraceComplex = false;
	FVector SpawnLocation = Probe + FVector(0.0, 0.0, 18.0);
	if (World->LineTraceSingleByObjectType(
			GroundHit,
			Probe + FVector(0.0, 0.0, 10000.0),
			Probe - FVector(0.0, 0.0, 10000.0),
			FCollisionObjectQueryParams(ECC_WorldStatic),
			QueryParams)
		&& GroundHit.bBlockingHit)
	{
		SpawnLocation = GroundHit.ImpactPoint + FVector(0.0, 0.0, 18.0);
	}

	ATargetPoint* Robot = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform(SpawnLocation));
	if (!Robot)
	{
		UE_LOG(LogUERLEgyptChase, Error, TEXT("Egypt chase failed to spawn a policy owner"));
		bEgyptMap = false;
		return;
	}
	UUERLPolicyComponent* Component = NewObject<UUERLPolicyComponent>(Robot);
	Robot->AddInstanceComponent(Component);
	Component->Artifact = Artifact;
	Component->bClaimOwnerMesh = false;
	Component->bAutoStart = true;
	Component->RegisterComponent();
	if (Robot->HasActorBegunPlay() && !Component->HasBegunPlay())
	{
		Component->BeginPlay();
	}
	Component->SetCommand(TEXT("velocity"), {0.0f, 0.0f, 0.0f});
	PolicyComponent = Component;
	UE_LOG(LogUERLEgyptChase, Display, TEXT("Egypt chase spawned PhantomX at %s"), *SpawnLocation.ToString());
}

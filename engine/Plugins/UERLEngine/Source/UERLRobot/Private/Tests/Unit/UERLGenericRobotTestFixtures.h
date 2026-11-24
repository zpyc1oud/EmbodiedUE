#pragma once

#include "Misc/AutomationTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "UERLBatchBinding.h"
#include "UERLGenericRobotProvider.h"
#include "UERLSkeletalMeshRobotRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UERLGenericRobotTestsPrivate
{
	inline constexpr TCHAR CartPoleRobotAsset[] = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");
	inline constexpr TCHAR PhantomXRobotAsset[] = TEXT("/Game/Robots/PhantomX/SK_PhantomX.SK_PhantomX");

	inline FUERLBatchSchema SelectAllFields(const FUERLRobotDescriptor& Descriptor)
	{
		FUERLBatchSchema Schema;
		for (const FUERLFieldDescriptor& Field : Descriptor.ActionFields)
		{
			FUERLBatchFieldBinding& Binding = Schema.ActionFields.AddDefaulted_GetRef();
			Binding.Field = Field;
			Binding.Column = Schema.ActionWidth;
			Schema.ActionWidth += Field.Width;
		}
		for (const FUERLFieldDescriptor& Field : Descriptor.StateFields)
		{
			FUERLBatchFieldBinding& Binding = Schema.StateFields.AddDefaulted_GetRef();
			Binding.Field = Field;
			Binding.Column = Schema.StateWidth;
			Schema.StateWidth += Field.Width;
		}
		return Schema;
	}

	inline TArray<FUERLFieldDescriptor> FieldsFromSchema(const FUERLBatchSchema& Schema)
	{
		TArray<FUERLFieldDescriptor> Fields;
		Fields.Reserve(Schema.StateFields.Num());
		for (const FUERLBatchFieldBinding& Binding : Schema.StateFields)
		{
			Fields.Add(Binding.Field);
		}
		return Fields;
	}

	inline AActor* SpawnGround(UWorld& World)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube)
		{
			return nullptr;
		}
		AStaticMeshActor* Ground = World.SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, -50.0), FRotator::ZeroRotator);
		if (!Ground)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = Ground->GetStaticMeshComponent();
		Component->SetStaticMesh(Cube);
		Component->SetMobility(EComponentMobility::Static);
		Component->SetCollisionObjectType(ECC_WorldStatic);
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetWorldScale3D(FVector(10.0, 10.0, 0.5));
		return Ground;
	}

	inline AActor* SpawnWorldStaticBox(UWorld& World, const FVector& Location, const FVector& Scale)
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

	/** PhantomX position actuators with the Python asset gains and reference pose. */
	inline FUERLSkeletalMeshRobotRuntimeConfig PhantomXRuntimeConfig(const FVector& GroundOrigin)
	{
		FUERLSkeletalMeshRobotRuntimeConfig Config;
		Config.AssetPath = PhantomXRobotAsset;
		Config.bClaimAuthoredActor = false;
		Config.GroundOrigin = GroundOrigin;
		Config.InitialRootHeightMeters = 0.18;
		for (const TCHAR* Leg : { TEXT("rf"), TEXT("rm"), TEXT("rr"), TEXT("lf"), TEXT("lm"), TEXT("lr") })
		{
			for (const TPair<const TCHAR*, double>& Segment : {
				TPair<const TCHAR*, double>(TEXT("c1"), 0.0),
				TPair<const TCHAR*, double>(TEXT("thigh"), 0.15),
				TPair<const TCHAR*, double>(TEXT("tibia"), -0.30) })
			{
				FUERLSkeletalMeshRuntimeActuator& Actuator = Config.Actuators.AddDefaulted_GetRef();
				Actuator.JointName = FName(*FString::Printf(TEXT("%s_%s"), Segment.Key, Leg));
				Actuator.Stiffness = 25.0;
				Actuator.Damping = 0.5;
				Actuator.EffortLimit = 2.8;
				Actuator.DefaultPosition = Segment.Value;
			}
		}
		return Config;
	}

	inline USkeletalMeshComponent* FindSpawnedRobotMesh(UWorld& World)
	{
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			USkeletalMeshComponent* Candidate = It->FindComponentByClass<USkeletalMeshComponent>();
			if (Candidate && Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh")))
			{
				return Candidate;
			}
		}
		return nullptr;
	}
}

#endif

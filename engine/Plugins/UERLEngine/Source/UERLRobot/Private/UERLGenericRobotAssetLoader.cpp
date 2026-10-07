#include "UERLGenericRobotAssetLoader.h"

#include "UERLGenericRobotCommandApplier.h"
#include "UERLInterfaceTypes.h"
#include "UERLProvider.h"

#include "Chaos/RigidParticles.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "UObject/UObjectGlobals.h"

bool ResolveGenericRobotRuntimeConstraintIndices(
	USkeletalMeshComponent& Component,
	const FUERLRobotTopology& Topology,
	TArray<int32>& OutIndices,
	FString& OutError)
{
	OutIndices.Init(INDEX_NONE, Topology.Joints.Num());
	TSet<int32> UsedConstraintIndices;
	for (int32 JointIndex = 0; JointIndex < Topology.Joints.Num(); ++JointIndex)
	{
		const FUERLJointTopology& Joint = Topology.Joints[JointIndex];
		if (!Topology.BodyNames.IsValidIndex(Joint.ChildBodyIndex)
			|| !Topology.BodyNames.IsValidIndex(Joint.ParentBodyIndex))
		{
			OutError = FString::Printf(TEXT("generic Robot joint %d references an invalid body"), JointIndex);
			return false;
		}
		int32 MatchIndex = INDEX_NONE;
		for (int32 ConstraintIndex = 0;; ++ConstraintIndex)
		{
			FConstraintInstance* Instance = Component.GetConstraintInstanceByIndex(ConstraintIndex);
			if (!Instance)
			{
				break;
			}
			if (Instance->JointName == Joint.Name
				&& Instance->ConstraintBone1 == Topology.BodyNames[Joint.ChildBodyIndex]
				&& Instance->ConstraintBone2 == Topology.BodyNames[Joint.ParentBodyIndex])
			{
				if (MatchIndex != INDEX_NONE)
				{
					OutError = FString::Printf(
						TEXT("generic Robot runtime has duplicate constraint identity for joint '%s'"),
						*Joint.Name.ToString());
					return false;
				}
				MatchIndex = ConstraintIndex;
			}
		}
		if (MatchIndex == INDEX_NONE)
		{
			OutError = FString::Printf(
				TEXT("generic Robot runtime has no constraint matching joint '%s'"),
				*Joint.Name.ToString());
			return false;
		}
		if (UsedConstraintIndices.Contains(MatchIndex))
		{
			OutError = FString::Printf(
				TEXT("generic Robot runtime maps multiple topology joints to constraint index %d"), MatchIndex);
			return false;
		}
		UsedConstraintIndices.Add(MatchIndex);
		OutIndices[JointIndex] = MatchIndex;
	}
	return true;
}

void DestroyGenericRobotSpawnedSlots(TArray<FUERLGenericRobotSpawnedSlot>& Slots)
{
	for (const FUERLGenericRobotSpawnedSlot& Slot : Slots)
	{
		if (Slot.bHasClaimedSettings && Slot.Component.IsValid())
		{
			USkeletalMeshComponent* Component = Slot.Component.Get();
			for (const TPair<FName, Chaos::ESleepType>& SleepType : Slot.ClaimedSleepTypes)
			{
				const FBodyInstance* Body = Component->GetBodyInstance(SleepType.Key);
				if (FPhysicsActorHandle Handle = Body ? Body->GetPhysicsActorHandle() : nullptr)
				{
					Handle->GetGameThreadAPI().SetSleepType(SleepType.Value);
				}
			}
			if (Component->GetPhysicsMaterialOverride() != Slot.ClaimedPhysMaterialOverride.Get())
			{
				Component->SetPhysMaterialOverride(Slot.ClaimedPhysMaterialOverride.Get());
			}
			Component->SetSimulatePhysics(Slot.bClaimedSimulatePhysics);
			Component->SetComponentTickEnabled(Slot.bClaimedTickEnabled);
			Component->SetEnableGravity(Slot.bClaimedGravityEnabled);
			Component->SetNotifyRigidBodyCollision(Slot.bClaimedNotifyRigidBodyCollision);
			Component->SetCollisionEnabled(Slot.ClaimedCollisionEnabled);
			if (!Slot.ClaimedCollisionProfile.IsNone())
			{
				Component->SetCollisionProfileName(Slot.ClaimedCollisionProfile);
			}
			Component->SetMobility(Slot.ClaimedMobility);
			if (Slot.ClaimedAttachParent.IsValid())
			{
				Component->AttachToComponent(
					Slot.ClaimedAttachParent.Get(),
					FAttachmentTransformRules::SnapToTargetNotIncludingScale);
				Component->SetRelativeTransform(Slot.ClaimedRelativeTransform);
			}
			if (Slot.Owner.IsValid())
			{
				Slot.Owner->SetActorTransform(Slot.ClaimedTransform, false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
		if (Slot.ContactListener.IsValid())
		{
			Slot.ContactListener->Unbind();
			if (!Slot.bOwnsActor)
			{
				Slot.ContactListener->DestroyComponent();
			}
		}
		if (Slot.bOwnsActor && Slot.Owner.IsValid())
		{
			Slot.Owner->Destroy();
		}
	}
	Slots.Reset();
}

bool SpawnGenericRobotSlots(
	UWorld& World,
	const FString& AssetPath,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLActuatorConfig> Actuators,
	bool bClaimAuthoredActor,
	const TArray<FUERLSlotContext>& SlotContexts,
	TArray<FUERLGenericRobotSpawnedSlot>& OutSlots,
	FString& OutError,
	USkeletalMeshComponent* ExplicitClaimedComponent,
	const FTransform* ExplicitPlacementTransform)
{
	DestroyGenericRobotSpawnedSlots(OutSlots);
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *AssetPath);
	UPhysicsAsset* PhysicsAsset = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!Mesh || !PhysicsAsset)
	{
		OutError = TEXT("generic Robot asset could not load a SkeletalMesh with a PhysicsAsset");
		return false;
	}
	AActor* AuthoredOwner = nullptr;
	USkeletalMeshComponent* AuthoredComponent = nullptr;
	if (bClaimAuthoredActor)
	{
		if (SlotContexts.Num() != 1)
		{
			OutError = TEXT("authored generic Robot requires exactly one Slot");
			return false;
		}
		if (ExplicitClaimedComponent)
		{
			if (ExplicitClaimedComponent->GetWorld() != &World
				|| ExplicitClaimedComponent->GetSkeletalMeshAsset() != Mesh)
			{
				OutError = TEXT("explicit claimed SkeletalMesh component does not belong to the World or asset");
				return false;
			}
			AuthoredComponent = ExplicitClaimedComponent;
			AuthoredOwner = ExplicitClaimedComponent->GetOwner();
		}
		for (TActorIterator<AActor> ActorIt(&World); !AuthoredComponent && ActorIt; ++ActorIt)
		{
			TInlineComponentArray<USkeletalMeshComponent*> Components(*ActorIt);
			for (USkeletalMeshComponent* Candidate : Components)
			{
				if (Candidate && Candidate->GetSkeletalMeshAsset() == Mesh)
				{
					if (AuthoredComponent)
					{
						OutError = TEXT("authored generic Robot found more than one matching SkeletalMesh component");
						return false;
					}
					AuthoredOwner = *ActorIt;
					AuthoredComponent = Candidate;
				}
			}
		}
		if (!AuthoredOwner || !AuthoredComponent)
		{
			OutError = TEXT("authored generic Robot found no matching SkeletalMesh component");
			return false;
		}
	}

	OutSlots.Reserve(SlotContexts.Num());
	for (const FUERLSlotContext& Context : SlotContexts)
	{
		if (Context.SlotId != OutSlots.Num())
		{
			OutError = TEXT("generic Robot requires contiguous Slot identities");
			DestroyGenericRobotSpawnedSlots(OutSlots);
			return false;
		}
		checkf(
			Context.CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld
				|| !Context.EnvironmentActors.IsEmpty(),
			TEXT("isolated generic Robot Slot has no Environment owners"));
		const FQuat GroundRotation = FQuat::FindBetweenNormals(
			FVector::UpVector, Context.GroundNormal.GetSafeNormal());
		AActor* Owner = AuthoredOwner;
		USkeletalMeshComponent* Component = AuthoredComponent;
		const bool bOwnsActor = !bClaimAuthoredActor;
		if (bOwnsActor)
		{
			Owner = World.SpawnActor<AActor>(
				AActor::StaticClass(), Context.Origin, GroundRotation.Rotator());
		}
		if (!Owner)
		{
			OutError = FString::Printf(TEXT("failed to spawn generic Robot Actor for Slot %d"), Context.SlotId);
			DestroyGenericRobotSpawnedSlots(OutSlots);
			return false;
		}

		if (bOwnsActor)
		{
			Component = NewObject<USkeletalMeshComponent>(Owner, TEXT("RobotSkeletalMesh"));
			Owner->SetRootComponent(Component);
		}
		checkf(Component, TEXT("generic Robot SkeletalMesh component is missing"));
		const FVector PlacementOrigin = bOwnsActor
			? (ExplicitPlacementTransform ? ExplicitPlacementTransform->GetLocation() : Context.Origin)
			: Owner->GetActorLocation();
		const FQuat PlacementRotation = bOwnsActor
			? (ExplicitPlacementTransform ? ExplicitPlacementTransform->GetRotation() : GroundRotation)
			: Owner->GetActorQuat();
		const FTransform ClaimedTransform = bOwnsActor ? FTransform::Identity : Owner->GetActorTransform();
		const FName ClaimedCollisionProfile = bOwnsActor ? NAME_None : Component->GetCollisionProfileName();
		const TEnumAsByte<EComponentMobility::Type> ClaimedMobility = Component->Mobility;
		const TEnumAsByte<ECollisionEnabled::Type> ClaimedCollisionEnabled = Component->GetCollisionEnabled();
		const FBodyInstance* RootBodyInstance = Component->GetBodyInstance();
		const bool bClaimedNotifyRigidBodyCollision = RootBodyInstance
			? RootBodyInstance->bNotifyRigidBodyCollision : false;
		const bool bClaimedGravityEnabled = Component->IsGravityEnabled();
		const bool bClaimedTickEnabled = Component->IsComponentTickEnabled();
		const bool bClaimedSimulatePhysics = Component->IsSimulatingPhysics();
		USceneComponent* const ClaimedAttachParent = Component->GetAttachParent();
		const FTransform ClaimedRelativeTransform = Component->GetRelativeTransform();
		const FQuat SlotGroundRotation = bOwnsActor
			? GroundRotation
			: FRotator(0.0f, Owner->GetActorRotation().Yaw, 0.0f).Quaternion();
		if (bOwnsActor)
		{
			Owner->SetActorLocationAndRotation(
				PlacementOrigin, PlacementRotation.Rotator(), false, nullptr, ETeleportType::TeleportPhysics);
		}
		Component->SetMobility(EComponentMobility::Movable);
		if (bOwnsActor)
		{
			Component->SetWorldLocationAndRotation(PlacementOrigin, PlacementRotation);
			Component->SetSkeletalMeshAsset(Mesh);
			Component->SetPhysicsAsset(PhysicsAsset, true);
		}
		Context.CollisionProfile.ApplyRobot(*Component);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetNotifyRigidBodyCollision(true);
		if (!Component->IsRegistered())
		{
			Component->RegisterComponent();
		}
		if (!bOwnsActor)
		{
			Component->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		}
		Component->SetAllBodiesSimulatePhysics(false);
		const bool bFloatingRoot = Topology.BodyMotionTypes.IsValidIndex(Topology.RootBodyIndex)
			&& Topology.BodyMotionTypes[Topology.RootBodyIndex] == EUERLBodyMotionType::Simulated;
		if (bFloatingRoot)
		{
			Component->PhysicsTransformUpdateMode = EPhysicsTransformUpdateMode::SimulationUpatesComponentTransform;
		}
		Component->SetSimulatePhysics(bFloatingRoot);
		for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
		{
			const bool bSimulated = Topology.BodyMotionTypes.IsValidIndex(BodyIndex)
				&& Topology.BodyMotionTypes[BodyIndex] == EUERLBodyMotionType::Simulated;
			Component->SetBodySimulatePhysics(Topology.BodyNames[BodyIndex], bSimulated);
		}
		if (bFloatingRoot)
		{
			Component->PrimaryComponentTick.bCanEverTick = true;
			Component->SetComponentTickEnabled(true);
			Component->PhysicsTransformUpdateMode = EPhysicsTransformUpdateMode::SimulationUpatesComponentTransform;
			if (UPhysicsAsset* PhysicsAssetForRoot = Component->GetPhysicsAsset())
			{
				const int32 RootPhysicsBodyIndex = PhysicsAssetForRoot->FindBodyIndex(
					Topology.BodyNames[Topology.RootBodyIndex]);
				if (RootPhysicsBodyIndex != INDEX_NONE)
				{
					Component->SetRootBodyIndex(RootPhysicsBodyIndex);
				}
			}
			Component->RegisterEndPhysicsTick(true);
		}
		Component->SetEnableGravity(true);
		FUERLGenericRobotSpawnedSlot& Slot = OutSlots.AddDefaulted_GetRef();
		Slot.Owner = Owner;
		Slot.Component = Component;
		Slot.bOwnsActor = bOwnsActor;
		Slot.bPreserveOwnerTransform = !bOwnsActor;
		Slot.ClaimedTransform = ClaimedTransform;
		Slot.ClaimedMobility = ClaimedMobility;
		Slot.ClaimedCollisionEnabled = ClaimedCollisionEnabled;
		Slot.ClaimedCollisionProfile = ClaimedCollisionProfile;
		Slot.bClaimedNotifyRigidBodyCollision = bClaimedNotifyRigidBodyCollision;
		Slot.bClaimedGravityEnabled = bClaimedGravityEnabled;
		Slot.bClaimedTickEnabled = bClaimedTickEnabled;
		Slot.bHasClaimedSettings = !bOwnsActor;
		Slot.bClaimedSimulatePhysics = bClaimedSimulatePhysics;
		Slot.ClaimedAttachParent = ClaimedAttachParent;
		Slot.ClaimedRelativeTransform = ClaimedRelativeTransform;
		Slot.ClaimedPhysMaterialOverride.Reset(bOwnsActor ? nullptr : Component->GetPhysicsMaterialOverride());
		for (const FName BodyName : Topology.BodyNames)
		{
			FBodyInstance* Body = Component->GetBodyInstance(BodyName);
			if (!Body || !Context.CollisionProfile.ApplyRobotBody(*Body))
			{
				OutError = FString::Printf(
					TEXT("generic Robot body '%s' has no initialized physics actor for Slot %d"),
					*BodyName.ToString(), Context.SlotId);
				DestroyGenericRobotSpawnedSlots(OutSlots);
				return false;
			}
		}

		Slot.Origin = PlacementOrigin;
		Slot.GroundRotation = SlotGroundRotation;
		Slot.CollisionProfile = Context.CollisionProfile;
		Slot.TerrainQueryActors = Context.TerrainQueryActors;
		Slot.TerrainQueryPurpose = Context.TerrainQueryPurpose;
		Slot.bWorldUpObservationFrame =
			Context.TerrainQueryPurpose == EUERLTerrainQueryPurpose::DeploymentWorldStatic;
		Slot.TerrainHalfExtent = Context.TerrainHalfExtent;
		Slot.TerrainBoundsOriginOffset = Context.TerrainBoundsOriginOffset;
		Slot.bTerrainBoundsValid = Context.bTerrainBoundsValid;
		Slot.Targets.SetNum(Actuators.Num());
		for (int32 ActuatorIndex = 0; ActuatorIndex < Actuators.Num(); ++ActuatorIndex)
		{
			Slot.Targets[ActuatorIndex] = Actuators[ActuatorIndex].TargetMode == TEXT("position")
				? static_cast<float>(Actuators[ActuatorIndex].DefaultPosition) : 0.0f;
		}
		Slot.BoneIndices.Reserve(Topology.BodyNames.Num());
		for (const FName BodyName : Topology.BodyNames)
		{
			const int32 BoneIndex = Component->GetBoneIndex(BodyName);
			if (BoneIndex == INDEX_NONE)
			{
				OutError = FString::Printf(
					TEXT("generic Robot SkeletalMesh has no reflected body bone '%s'"),
					*BodyName.ToString());
				DestroyGenericRobotSpawnedSlots(OutSlots);
				return false;
			}
			Slot.BoneIndices.Add(BoneIndex);
		}
		if (!ResolveGenericRobotRuntimeConstraintIndices(*Component, Topology, Slot.ConstraintIndices, OutError))
		{
			DestroyGenericRobotSpawnedSlots(OutSlots);
			return false;
		}
		for (const int32 ConstraintIndex : Slot.ConstraintIndices)
		{
			FConstraintInstance* Constraint = Component->GetConstraintInstanceByIndex(ConstraintIndex);
			checkf(Constraint, TEXT("generic Robot runtime constraint %d is missing"), ConstraintIndex);
			Constraint->SetLinearPositionDrive(false, false, false);
			Constraint->SetLinearVelocityDrive(false, false, false);
			Constraint->SetOrientationDriveTwistAndSwing(false, false);
			Constraint->SetOrientationDriveSLERP(false);
			Constraint->SetAngularVelocityDriveTwistAndSwing(false, false);
			Constraint->SetAngularVelocityDriveSLERP(false);
		}
		ConfigureGenericRobotRevoluteAngularDrives(*Component, Slot.ConstraintIndices, Actuators);
		Slot.ContactState = MakeUnique<FGenericRobotContactState>(Topology.BodyNames.Num());
		UERLGenericRobotContactListener* ContactListener = NewObject<UERLGenericRobotContactListener>(
			Owner, TEXT("RobotContactListener"));
		ContactListener->Bind(
			*Component,
			*Slot.ContactState,
			Context.CollisionProfile.Scope(),
			Context.EnvironmentActors,
			Topology.BodyNames);
		Owner->AddInstanceComponent(ContactListener);
		ContactListener->RegisterComponent();
		Slot.ContactListener = ContactListener;
		const FTransform SlotFrame(Slot.GroundRotation, Slot.Origin);
		// Floating root_pose is the mesh frame. A claimed mount belongs to its
		// world placement, not to the intrinsic body reference used by reset
		// and root observation recovery. Fixed bases retain the mounting frame.
		const FTransform CanonicalFrame = !bOwnsActor && bFloatingRoot
			? Component->GetComponentTransform() : SlotFrame;
		Slot.CanonicalBodies.Reserve(Topology.BodyNames.Num());
		for (const FName BodyName : Topology.BodyNames)
		{
			FBodyInstance* Body = Component->GetBodyInstance(BodyName);
			checkf(Body, TEXT("generic Robot canonical body '%s' is missing"), *BodyName.ToString());
			FUERLGenericRobotCanonicalBody& Canonical = Slot.CanonicalBodies.AddDefaulted_GetRef();
			Canonical.SlotTransform = Body->GetUnrealWorldTransform().GetRelativeTransform(CanonicalFrame);
			Canonical.SlotLinearVelocity = Slot.GroundRotation.Inverse().RotateVector(
				Body->GetUnrealWorldVelocity());
			Canonical.SlotAngularVelocity = Slot.GroundRotation.Inverse().RotateVector(
				Body->GetUnrealWorldAngularVelocityInRadians());
		}
		// Changing a Chaos joint drive target does not wake a sleeping island, so a
		// Robot that settles under an unchanged command would ignore the next one.
		for (const FName BodyName : Topology.BodyNames)
		{
			FPhysicsActorHandle Handle = Component->GetBodyInstance(BodyName)->GetPhysicsActorHandle();
			checkf(Handle, TEXT("generic Robot body '%s' has no physics actor"), *BodyName.ToString());
			if (!bOwnsActor)
			{
				Slot.ClaimedSleepTypes.Emplace(BodyName, Handle->GetGameThreadAPI().SleepType());
			}
			Handle->GetGameThreadAPI().SetSleepType(Chaos::ESleepType::NeverSleep);
		}
		Component->WakeAllRigidBodies();
		if (!FApp::CanEverRender())
		{
			Component->SetComponentTickEnabled(false);
			Component->SetAllBodiesPhysicsBlendWeight(0.0f);
		}
	}
	return true;
}

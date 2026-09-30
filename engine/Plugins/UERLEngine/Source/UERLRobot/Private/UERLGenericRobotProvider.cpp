#include "UERLGenericRobotProvider.h"

#include "UERLBatchBinding.h"
#include "UERLGenericRobotAssetLoader.h"
#include "UERLGenericRobotContactListener.h"
#include "UERLGenericRobotCommandApplier.h"
#include "UERLGenericRobotObservationSampler.h"
#include "UERLGenericRobotResetApplier.h"
#include "UERLGenericRobotStateFields.h"
#include "UERLRobotLog.h"
#include "UERLRobotObservationPlan.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace UERLGenericRobot
{
	const FName RobotId(TEXT("uerl.robot.skeletal_mesh"));
	const FName ClaimAuthoredActor(TEXT("robot.claim_authored_actor"));
	const FName SolverStepCommands(TEXT("robot.solver_step_commands"));
}
namespace
{
	class FUERLGenericRobot final : public IUERLRobot
	{
		struct FSlot
		{
			TWeakObjectPtr<AActor> Owner;
			TWeakObjectPtr<USkeletalMeshComponent> Component;
			TWeakObjectPtr<UERLGenericRobotContactListener> ContactListener;
			TUniquePtr<FGenericRobotContactState> ContactState;
			TArray<int32> BoneIndices;
			TArray<int32> ConstraintIndices;
			FVector Origin = FVector::ZeroVector;
			FQuat GroundRotation = FQuat::Identity;
			FUERLSlotCollisionProfile CollisionProfile;
			TArray<TWeakObjectPtr<AActor>> TerrainQueryActors;
			EUERLTerrainQueryPurpose TerrainQueryPurpose = EUERLTerrainQueryPurpose::TrainingOwned;
			bool bWorldUpObservationFrame = false;
			FVector2D TerrainHalfExtent = FVector2D::ZeroVector;
			FVector TerrainBoundsOriginOffset = FVector::ZeroVector;
			bool bTerrainBoundsValid = false;
			mutable bool bTerrainBoundary = false;
			TArray<FUERLGenericRobotCanonicalBody> CanonicalBodies;
			TArray<float> Targets;
			TUniquePtr<FUERLGenericRobotSolverStepCommands> SolverStepCommands;
			mutable TArray<FHitResult> SupportTraceHits;
			mutable TArray<FHitResult> TerrainTraceHits;
			mutable TArray<float> TerrainHeightValues;
			mutable TArray<FVector> TerrainHeightWorldHits;
			mutable TArray<uint8> TerrainHeightHitValid;
			mutable TArray<FVector> GroundClearanceWorldHits;
			mutable TArray<uint8> GroundClearanceHitValid;
			bool bOwnsActor = true;
			bool bPreserveOwnerTransform = false;
			FTransform ClaimedTransform = FTransform::Identity;
			TEnumAsByte<EComponentMobility::Type> ClaimedMobility = EComponentMobility::Movable;
			TEnumAsByte<ECollisionEnabled::Type> ClaimedCollisionEnabled = ECollisionEnabled::NoCollision;
			FName ClaimedCollisionProfile = NAME_None;
			bool bClaimedNotifyRigidBodyCollision = false;
			bool bClaimedGravityEnabled = true;
			bool bClaimedTickEnabled = true;
			bool bHasClaimedSettings = false;
			TWeakObjectPtr<USceneComponent> ClaimedAttachParent;
			FTransform ClaimedRelativeTransform = FTransform::Identity;
			bool bClaimedSimulatePhysics = false;
			TArray<TPair<FName, Chaos::ESleepType>> ClaimedSleepTypes;
			TWeakObjectPtr<UPhysicalMaterial> ClaimedPhysMaterialOverride;
		};

	public:
		FUERLGenericRobot(
			FString InAssetPath,
			FUERLRobotTopology InTopology,
			const TArray<FUERLFieldDescriptor>& InAvailableStateFields,
			TArray<FUERLActuatorConfig> InActuators,
			TArray<FUERLResetBinding> InResetBindings,
			bool bInClaimAuthoredActor,
			bool bInSolverStepCommands,
			USkeletalMeshComponent* InClaimedComponent,
			const FTransform& InPlacementTransform,
			bool bInHasPlacementTransform)
			: AssetPath(MoveTemp(InAssetPath))
			, Topology(MoveTemp(InTopology))
			, AvailableStateFields(InAvailableStateFields)
			, Actuators(MoveTemp(InActuators))
			, ResetBindings(MoveTemp(InResetBindings))
			, bClaimAuthoredActor(bInClaimAuthoredActor)
			, bSolverStepCommands(bInSolverStepCommands)
			, ClaimedComponent(InClaimedComponent)
			, PlacementTransform(InPlacementTransform)
			, bHasPlacementTransform(bInHasPlacementTransform)
		{
			ParentBodyIndices.Init(INDEX_NONE, Topology.BodyNames.Num());
			for (const FUERLJointTopology& Joint : Topology.Joints)
			{
				ParentBodyIndices[Joint.ChildBodyIndex] = Joint.ParentBodyIndex;
			}
			for (const FUERLActuatorConfig& Actuator : Actuators)
			{
				if (IsGenericRobotDriveActuator(Actuator))
				{
					DriveActuators.Add(Actuator);
				}
			}
		}

		virtual bool PrepareState(
			const TArray<FUERLFieldDescriptor>& SelectedStateFields,
			FString& OutError) override
		{
			TArray<FUERLFieldDescriptor> SelectedRobotFields;
			SelectRobotObservationFields(AvailableStateFields, SelectedStateFields, SelectedRobotFields);
			ContactBodyIndices.Reset();
			if (!CompileRobotObservationPlan(Topology, SelectedRobotFields, Plan, OutError))
			{
				return false;
			}
			for (const FUERLRobotObservationPlanEntry& Entry : Plan)
			{
				if (Entry.Type == EUERLObservationType::Contact
					|| Entry.Type == EUERLObservationType::ContactForce)
				{
					ContactBodyIndices.AddUnique(Entry.BodyIndex);
				}
			}
			return true;
		}

		virtual bool SpawnIntoSlots(
			UWorld& World,
			const TArray<FUERLSlotContext>& SlotContexts,
			FString& OutError) override
		{
			DestroySlots();
			TArray<FUERLGenericRobotSpawnedSlot> Spawned;
			if (!SpawnGenericRobotSlots(
				World,
				AssetPath,
				Topology,
				Actuators,
				bClaimAuthoredActor,
				SlotContexts,
				Spawned,
				OutError,
				ClaimedComponent,
				bHasPlacementTransform ? &PlacementTransform : nullptr))
			{
				return false;
			}
			Slots.Reserve(Spawned.Num());
			for (FUERLGenericRobotSpawnedSlot& Item : Spawned)
			{
				FSlot& Slot = Slots.AddDefaulted_GetRef();
				Slot.Owner = Item.Owner;
				Slot.Component = Item.Component;
				Slot.ContactListener = Item.ContactListener;
				Slot.ContactState = MoveTemp(Item.ContactState);
				Slot.BoneIndices = MoveTemp(Item.BoneIndices);
				Slot.ConstraintIndices = MoveTemp(Item.ConstraintIndices);
				Slot.Origin = Item.Origin;
				Slot.GroundRotation = Item.GroundRotation;
				Slot.CollisionProfile = Item.CollisionProfile;
				Slot.TerrainQueryActors = MoveTemp(Item.TerrainQueryActors);
				Slot.TerrainQueryPurpose = Item.TerrainQueryPurpose;
				Slot.bWorldUpObservationFrame = Item.bWorldUpObservationFrame;
				Slot.TerrainHalfExtent = Item.TerrainHalfExtent;
				Slot.TerrainBoundsOriginOffset = Item.TerrainBoundsOriginOffset;
				Slot.bTerrainBoundsValid = Item.bTerrainBoundsValid;
				Slot.CanonicalBodies = MoveTemp(Item.CanonicalBodies);
				Slot.Targets = MoveTemp(Item.Targets);
				Slot.TerrainHeightValues.SetNumUninitialized(UERLTerrainHeightScanWidth);
				Slot.TerrainHeightWorldHits.SetNumZeroed(UERLTerrainHeightScanWidth);
				Slot.TerrainHeightHitValid.SetNumZeroed(UERLTerrainHeightScanWidth);
				Slot.GroundClearanceWorldHits.SetNumZeroed(Topology.BodyNames.Num());
				Slot.GroundClearanceHitValid.SetNumZeroed(Topology.BodyNames.Num());
				Slot.bOwnsActor = Item.bOwnsActor;
				Slot.bPreserveOwnerTransform = Item.bPreserveOwnerTransform;
				Slot.ClaimedTransform = Item.ClaimedTransform;
				Slot.ClaimedMobility = Item.ClaimedMobility;
				Slot.ClaimedCollisionEnabled = Item.ClaimedCollisionEnabled;
				Slot.ClaimedCollisionProfile = Item.ClaimedCollisionProfile;
				Slot.bClaimedNotifyRigidBodyCollision = Item.bClaimedNotifyRigidBodyCollision;
				Slot.bClaimedGravityEnabled = Item.bClaimedGravityEnabled;
				Slot.bClaimedTickEnabled = Item.bClaimedTickEnabled;
				Slot.bHasClaimedSettings = Item.bHasClaimedSettings;
				Slot.bClaimedSimulatePhysics = Item.bClaimedSimulatePhysics;
				Slot.ClaimedAttachParent = Item.ClaimedAttachParent;
				Slot.ClaimedRelativeTransform = Item.ClaimedRelativeTransform;
				Slot.ClaimedSleepTypes = MoveTemp(Item.ClaimedSleepTypes);
				Slot.ClaimedPhysMaterialOverride = Item.ClaimedPhysMaterialOverride;
			}
			if (bSolverStepCommands && DriveActuators.Num() != Actuators.Num())
			{
				for (FSlot& Slot : Slots)
				{
					Slot.SolverStepCommands = MakeUnique<FUERLGenericRobotSolverStepCommands>();
					if (!Slot.SolverStepCommands->Register(
						World, MakeCommandSlotView(Slot), Topology, Actuators, OutError))
					{
						DestroySlots();
						return false;
					}
				}
			}
			return true;
		}

		virtual bool ApplyCommands(const FUERLNamedActionReader& Reader, FString& OutError) override
		{
			if (Reader.NumRows() != Slots.Num())
			{
				OutError = TEXT("generic SkeletalMesh Robot Action batch has the wrong number of rows");
				return false;
			}
			if (Actuators.IsEmpty())
			{
				OutError = TEXT("generic SkeletalMesh Robot has no indexed actuators");
				return false;
			}
			for (int32 SlotId = 0; SlotId < Slots.Num(); ++SlotId)
			{
				FSlot& Slot = Slots[SlotId];
				if (!Reader.ReadVector(SlotId, FName(TEXT("robot.actuator.target")), Slot.Targets))
				{
					OutError = TEXT("generic SkeletalMesh Robot could not read robot.actuator.target");
					return false;
				}
				if (Slot.SolverStepCommands)
				{
					if (!ApplyGenericRobotActuatorForces(MakeCommandSlotView(Slot), Topology, DriveActuators, OutError))
					{
						return false;
					}
					Slot.SolverStepCommands->PublishTargets(Slot.Targets);
				}
				else if (!ApplyGenericRobotActuatorForces(MakeCommandSlotView(Slot), Topology, Actuators, OutError))
				{
					return false;
				}
			}
			return true;
		}

		virtual bool RequiresCommandsEveryPhysicsFrame() const override
		{
			return !bSolverStepCommands && DriveActuators.Num() != Actuators.Num();
		}

		virtual void SamplePhysicsContacts(double SolverStepSeconds) override
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UERL_GenericRobot_SamplePhysicsContacts);
			if (ContactBodyIndices.IsEmpty())
			{
				return;
			}
			checkf(
				FMath::IsFinite(SolverStepSeconds) && SolverStepSeconds > 0.0,
				TEXT("generic Robot contact sample requires a positive solver-step dt"));
			for (FSlot& Slot : Slots)
			{
				checkf(Slot.ContactState.IsValid(), TEXT("generic Robot contact state is missing"));
				Slot.ContactState->BeginPhysicsSample();
				const FTransform RootCanonical = Slot.CanonicalBodies.IsValidIndex(Topology.RootBodyIndex)
					? Slot.CanonicalBodies[Topology.RootBodyIndex].SlotTransform
					: FTransform::Identity;
				for (const int32 BodyIndex : ContactBodyIndices)
				{
					if (IsGenericRobotBodySupportedByTerrain(
						*Slot.Component.Get(),
						*Slot.Owner.Get(),
						Topology,
						Slot.CollisionProfile,
						Slot.GroundRotation,
						RootCanonical,
						BodyIndex,
						Slot.SupportTraceHits))
					{
						Slot.ContactState->MarkBodySupported(BodyIndex);
					}
				}
				SampleContactForces(Slot, SolverStepSeconds);
			}
		}

		virtual bool ApplyEvent(const FUERLEventBatch& Event, FString& OutError) override
		{
			if (Event.Kind == EUERLEventKind::GroundFriction)
			{
				return ApplyGroundFriction(Event, OutError);
			}
			if (Event.Kind != EUERLEventKind::RootPush)
			{
				OutError = TEXT("generic SkeletalMesh Robot only accepts ground_friction and root_push events");
				return false;
			}
			if (!Topology.BodyMotionTypes.IsValidIndex(Topology.RootBodyIndex)
				|| Topology.BodyMotionTypes[Topology.RootBodyIndex] != EUERLBodyMotionType::Simulated)
			{
				OutError = TEXT("generic SkeletalMesh Robot root body is not simulated");
				return false;
			}
			const FName RootBodyName = Topology.BodyNames[Topology.RootBodyIndex];
			for (int32 Row = 0; Row < Event.SlotIds.Num(); ++Row)
			{
				const int32 SlotId = Event.SlotIds[Row];
				if (!Slots.IsValidIndex(SlotId) || !Slots[SlotId].Component.IsValid())
				{
					OutError = FString::Printf(TEXT("root_push references invalid Robot Slot %d"), SlotId);
					return false;
				}
				const FVector WorldVelocityDelta = Slots[SlotId].GroundRotation.RotateVector(Event.Values[Row]) * 100.0;
				Slots[SlotId].Component->AddImpulse(WorldVelocityDelta, RootBodyName, true);
			}
			return true;
		}

		virtual bool ResetSlots(const FUERLResetBatch& Reset, FString& OutError) override
		{
			TArray<FUERLRobotResetSlotRef> SlotRefs;
			SlotRefs.Reserve(Slots.Num());
			for (FSlot& Slot : Slots)
			{
				FUERLRobotResetSlotRef& Ref = SlotRefs.AddDefaulted_GetRef();
				Ref.Owner = Slot.Owner.Get();
				Ref.Component = Slot.Component.Get();
				Ref.BoneIndices = &Slot.BoneIndices;
				Ref.ConstraintIndices = &Slot.ConstraintIndices;
				Ref.Origin = &Slot.Origin;
				Ref.GroundRotation = &Slot.GroundRotation;
				Ref.CanonicalBodies = &Slot.CanonicalBodies;
				Ref.Targets = &Slot.Targets;
				Ref.ContactState = Slot.ContactState.Get();
				Ref.bTerrainBoundary = &Slot.bTerrainBoundary;
				Ref.bPreserveOwnerTransform = Slot.bPreserveOwnerTransform;
			}
			const bool bReset = ResetGenericRobotSlots(
				SlotRefs, Topology, ParentBodyIndices, Actuators, ResetBindings, Reset, OutError);
			if (bReset)
			{
				for (const FUERLResetRow& Row : Reset.Rows)
				{
					if (!Slots.IsValidIndex(Row.SlotId))
					{
						continue;
					}
					FSlot& Slot = Slots[Row.SlotId];
					for (uint8& Valid : Slot.TerrainHeightHitValid)
					{
						Valid = 0;
					}
					for (uint8& Valid : Slot.GroundClearanceHitValid)
					{
						Valid = 0;
					}
					if (Slot.SolverStepCommands)
					{
						Slot.SolverStepCommands->PublishTargets(Slot.Targets);
					}
				}
			}
			return bReset;
		}

		virtual bool GetPrimaryActorTransform(FTransform& OutTransform, FString& OutError) const override
		{
			if (Slots.IsEmpty() || !Slots[0].Component.IsValid())
			{
				OutTransform = FTransform::Identity;
				OutError = TEXT("generic Robot has no primary Slot mesh");
				return false;
			}
			OutTransform = Slots[0].Component->GetComponentTransform();
			if (OutTransform.ContainsNaN())
			{
				OutError = TEXT("generic Robot primary Slot mesh transform is non-finite");
				return false;
			}
			OutError.Reset();
			return true;
		}

		virtual void ClearContactState() override
		{
			for (FSlot& Slot : Slots)
			{
				if (Slot.ContactState.IsValid())
				{
					Slot.ContactState->Clear();
				}
			}
		}

		virtual void ClearObservationCaches() override
		{
			ClearContactState();
			for (FSlot& Slot : Slots)
			{
				for (uint8& Valid : Slot.TerrainHeightHitValid)
				{
					Valid = 0;
				}
				for (uint8& Valid : Slot.GroundClearanceHitValid)
				{
					Valid = 0;
				}
			}
		}

		virtual void CollectState(const TArray<int32>& SlotIds, FUERLNamedStateWriter& Writer) const override
		{
			FString Error;
			if (!TryCollectState(SlotIds, Writer, Error))
			{
				UE_LOG(LogUERLRobot, Error, TEXT("generic Robot State collection failed: %s"), *Error);
			}
		}

		virtual bool TryCollectState(
			const TArray<int32>& SlotIds,
			FUERLNamedStateWriter& Writer,
			FString& OutError) const override
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UERL_GenericRobot_CollectState);
			for (const int32 SlotId : SlotIds)
			{
				checkf(
					Slots.IsValidIndex(SlotId) && Slots[SlotId].Component.IsValid(),
					TEXT("generic Robot cannot collect State for invalid Slot %d"), SlotId);
				const FSlot& Slot = Slots[SlotId];
				// State collection runs after the physics step.  Terminate only when
				// the root reference point itself has left the generated tile; do not
				// predict a boundary from the Robot footprint or a forward probe.
				if (Slot.bTerrainBoundsValid && IsTerrainBoundary(Slot))
				{
					Slot.bTerrainBoundary = true;
					continue;
				}
				if (!WriteGenericRobotObservationFields(
					SlotId, MakeObservationSlotView(Slot), Topology, Plan, Writer, OutError))
				{
					return false;
				}
			}
			OutError.Reset();
			return true;
		}

		virtual EUERLSlotFaultCode ValidateSlot(int32 SlotId, FString& OutReason) const override
		{
			const FSlot* Slot = Slots.IsValidIndex(SlotId) ? &Slots[SlotId] : nullptr;
			if (!Slot || !Slot->Owner.IsValid()
				|| !Slot->Component.IsValid() || !Slot->Component->IsRegistered()
				|| !Slot->ContactListener.IsValid() || !Slot->ContactListener->IsRegistered()
				|| !Slot->ContactState.IsValid()
				|| Slot->BoneIndices.Num() != Topology.BodyNames.Num()
				|| Slot->ConstraintIndices.Num() != Topology.Joints.Num())
			{
				OutReason = TEXT("generic Robot Actor, component, or reflected bone is missing");
				return EUERLSlotFaultCode::MissingObject;
			}
			const FTransform Transform = Slot->Component->GetComponentTransform();
			if (Transform.ContainsNaN())
			{
				OutReason = TEXT("generic Robot component transform is non-finite");
				return EUERLSlotFaultCode::PhysicsStateInvalid;
			}
			if (Slot->bTerrainBoundary)
			{
				OutReason = TEXT("generic Robot reached the boundary of its generated terrain cell");
				return EUERLSlotFaultCode::TerrainBoundary;
			}
			return EUERLSlotFaultCode::None;
		}

		virtual void DestroySlots() override
		{
			TArray<FUERLGenericRobotSpawnedSlot> Spawned;
			Spawned.Reserve(Slots.Num());
			for (FSlot& Slot : Slots)
			{
				// The solver callback holds this Slot's physics handles; release it
				// in the same frame and before the bodies are destroyed.
				Slot.SolverStepCommands.Reset();
				FUERLGenericRobotSpawnedSlot& Item = Spawned.AddDefaulted_GetRef();
				Item.Owner = Slot.Owner;
				Item.Component = Slot.Component;
				Item.ContactListener = Slot.ContactListener;
				Item.bOwnsActor = Slot.bOwnsActor;
				Item.bPreserveOwnerTransform = Slot.bPreserveOwnerTransform;
				Item.ClaimedTransform = Slot.ClaimedTransform;
				Item.ClaimedMobility = Slot.ClaimedMobility;
				Item.ClaimedCollisionEnabled = Slot.ClaimedCollisionEnabled;
				Item.ClaimedCollisionProfile = Slot.ClaimedCollisionProfile;
				Item.bClaimedNotifyRigidBodyCollision = Slot.bClaimedNotifyRigidBodyCollision;
				Item.bClaimedGravityEnabled = Slot.bClaimedGravityEnabled;
				Item.bClaimedTickEnabled = Slot.bClaimedTickEnabled;
				Item.bHasClaimedSettings = Slot.bHasClaimedSettings;
				Item.bClaimedSimulatePhysics = Slot.bClaimedSimulatePhysics;
				Item.ClaimedAttachParent = Slot.ClaimedAttachParent;
				Item.ClaimedRelativeTransform = Slot.ClaimedRelativeTransform;
				Item.ClaimedSleepTypes = MoveTemp(Slot.ClaimedSleepTypes);
				Item.ClaimedPhysMaterialOverride = Slot.ClaimedPhysMaterialOverride;
			}
			Slots.Reset();
			DestroyGenericRobotSpawnedSlots(Spawned);
		}

	private:
		/**
		 * Give each Slot's bodies its own Multiply-combined friction material; the
		 * Environment makes the ground neutral so contacts use exactly these values.
		 */
		bool ApplyGroundFriction(const FUERLEventBatch& Event, FString& OutError)
		{
			const FName RootBodyName = Topology.BodyNames[Topology.RootBodyIndex];
			for (int32 Row = 0; Row < Event.SlotIds.Num(); ++Row)
			{
				const int32 SlotId = Event.SlotIds[Row];
				if (!Slots.IsValidIndex(SlotId) || !Slots[SlotId].Component.IsValid())
				{
					OutError = FString::Printf(TEXT("ground_friction references invalid Robot Slot %d"), SlotId);
					return false;
				}
			}
			for (int32 Row = 0; Row < Event.SlotIds.Num(); ++Row)
			{
				USkeletalMeshComponent* Component = Slots[Event.SlotIds[Row]].Component.Get();
				const FBodyInstance* RootBody = Component->GetBodyInstance(RootBodyName);
				UPhysicalMaterial* Material = NewObject<UPhysicalMaterial>(
					Component, NAME_None, RF_Transient, RootBody ? RootBody->GetSimplePhysicalMaterial() : nullptr);
				Material->StaticFriction = static_cast<float>(Event.Values[Row].X);
				Material->Friction = static_cast<float>(Event.Values[Row].Y);
				Material->FrictionCombineMode = EFrictionCombineMode::Multiply;
				Material->bOverrideFrictionCombineMode = true;
				Component->SetPhysMaterialOverride(Material);
			}
			return true;
		}

		void SampleContactForces(FSlot& Slot, double SolverStepSeconds)
		{
			checkf(Slot.Component.IsValid(), TEXT("generic Robot contact force sample has no component"));
			SampleGenericRobotContactForces(
				*Slot.Component.Get(),
				Topology.BodyNames,
				*Slot.ContactState,
				Slot.CollisionProfile.Scope(),
				SolverStepSeconds);
		}

		FUERLRobotCommandSlotView MakeCommandSlotView(const FSlot& Slot) const
		{
			FUERLRobotCommandSlotView View;
			View.Component = Slot.Component.Get();
			View.ConstraintIndices = &Slot.ConstraintIndices;
			View.Targets = &Slot.Targets;
			return View;
		}

		FUERLRobotObservationSlotView MakeObservationSlotView(const FSlot& Slot) const
		{
			FUERLRobotObservationSlotView View;
			View.Component = Slot.Component.Get();
			View.Owner = Slot.Owner.Get();
			View.ConstraintIndices = &Slot.ConstraintIndices;
			View.Origin = Slot.Origin;
			View.GroundRotation = Slot.GroundRotation;
			View.ObservationRotation = Slot.bWorldUpObservationFrame
				? FQuat::Identity : Slot.GroundRotation;
			View.CollisionProfile = Slot.CollisionProfile;
			View.TerrainQueryActors = &Slot.TerrainQueryActors;
			View.TerrainQueryPurpose = Slot.TerrainQueryPurpose;
			View.ContactState = Slot.ContactState.Get();
			if (Slot.CanonicalBodies.IsValidIndex(Topology.RootBodyIndex))
			{
				View.RootCanonicalSlotTransform = Slot.CanonicalBodies[Topology.RootBodyIndex].SlotTransform;
			}
			View.TerrainTraceHits = &Slot.TerrainTraceHits;
			View.TerrainHeightValues = &Slot.TerrainHeightValues;
			View.TerrainHeightWorldHits = &Slot.TerrainHeightWorldHits;
			View.TerrainHeightHitValid = &Slot.TerrainHeightHitValid;
			View.GroundClearanceWorldHits = &Slot.GroundClearanceWorldHits;
			View.GroundClearanceHitValid = &Slot.GroundClearanceHitValid;
			return View;
		}

		bool IsTerrainBoundary(const FSlot& Slot) const
		{
			checkf(Slot.bTerrainBoundsValid, TEXT("terrain boundary query requires generated terrain bounds"));
			const FTransform BoundsFrame(
				Slot.GroundRotation, Slot.Origin + Slot.TerrainBoundsOriginOffset);
			const FTransform BodyTransform = ObservedBodyWorldTransform(Slot, Topology.RootBodyIndex);
			const FVector LocalRoot = BoundsFrame.InverseTransformPosition(BodyTransform.GetLocation());
			return FMath::Abs(LocalRoot.X) >= Slot.TerrainHalfExtent.X
				|| FMath::Abs(LocalRoot.Y) >= Slot.TerrainHalfExtent.Y;
		}

		FTransform ObservedBodyWorldTransform(const FSlot& Slot, int32 BodyIndex) const
		{
			return ObserveGenericRobotBodyWorldTransform(
				MakeObservationSlotView(Slot), Topology, BodyIndex);
		}

		FString AssetPath;
		FUERLRobotTopology Topology;
		TArray<FUERLFieldDescriptor> AvailableStateFields;
		TArray<FUERLActuatorConfig> Actuators;
		TArray<FUERLActuatorConfig> DriveActuators;
		TArray<FUERLResetBinding> ResetBindings;
		TArray<FUERLRobotObservationPlanEntry> Plan;
		TArray<int32> ContactBodyIndices;
		TArray<int32> ParentBodyIndices;
		TArray<FSlot> Slots;
		bool bClaimAuthoredActor = false;
		bool bSolverStepCommands = false;
		USkeletalMeshComponent* ClaimedComponent = nullptr;
		FTransform PlacementTransform = FTransform::Identity;
		bool bHasPlacementTransform = false;
	};

	class FUERLGenericRobotFactory final : public IUERLRobotFactory
	{
	public:
		FUERLGenericRobotFactory()
		{
			Descriptor.Id = UERLGenericRobot::RobotId;
			Descriptor.Version = 1;
		}

		virtual const FUERLRobotDescriptor& Describe() const override { return Descriptor; }

		virtual bool ValidateConfig(
			const FUERLProviderConfig& Input,
			FUERLProviderConfig& OutEffective,
			FString& OutError) const override
		{
			return UERLGenericRobot::ValidateGenericRobotProviderConfig(
				Input, Descriptor, OutEffective, OutError);
		}

		virtual TUniquePtr<IUERLRobot> Create(const FUERLProviderConfig& EffectiveConfig) const override
		{
			return MakeUnique<FUERLGenericRobot>(
				EffectiveConfig.AssetPath, Descriptor.Topology, Descriptor.StateFields,
				EffectiveConfig.Actuators, EffectiveConfig.ResetBindings,
				EffectiveConfig.Scalars.FindRef(UERLGenericRobot::ClaimAuthoredActor) == 1.0,
				EffectiveConfig.Scalars.FindRef(UERLGenericRobot::SolverStepCommands) == 1.0,
				EffectiveConfig.ClaimedMesh,
				EffectiveConfig.PlacementTransform,
				EffectiveConfig.bHasPlacementTransform);
		}

	private:
		mutable FUERLRobotDescriptor Descriptor;
	};
}

TSharedRef<IUERLRobotFactory> UERLGenericRobot::MakeFactory()
{
	return MakeShared<FUERLGenericRobotFactory>();
}

#include "UERLSkeletalMeshRobotRuntime.h"

#include "UERLBatchBinding.h"
#include "UERLGenericRobotProvider.h"
#include "UERLProvider.h"
#include "UERLSlotCollisionPlan.h"

#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"

namespace
{
	const FUERLJointTopology* FindJoint(const FUERLRobotTopology& Topology, FName Name, int32& OutIndex)
	{
		for (int32 Index = 0; Index < Topology.Joints.Num(); ++Index)
		{
			if (Topology.Joints[Index].Name == Name)
			{
				OutIndex = Index;
				return &Topology.Joints[Index];
			}
		}
		OutIndex = INDEX_NONE;
		return nullptr;
	}

	const FUERLFieldDescriptor* FindObservation(
		const TArray<FUERLFieldDescriptor>& Fields,
		const FUERLRobotTopology& Topology,
		const FUERLSkeletalMeshRuntimeObservation& Request)
	{
		for (const FUERLFieldDescriptor& Field : Fields)
		{
			if (Field.Observation.Type != Request.Type)
			{
				continue;
			}
			if (Request.Type == EUERLObservationType::JointPosition
				|| Request.Type == EUERLObservationType::JointVelocity)
			{
				if (Topology.Joints.IsValidIndex(Field.Observation.JointIndex)
					&& Topology.Joints[Field.Observation.JointIndex].Name == Request.TargetName)
				{
					return &Field;
				}
			}
			else if (Field.Observation.BodyName == Request.TargetName)
			{
				return &Field;
			}
		}
		return nullptr;
	}

	FUERLResetBinding RootResetBinding(int32 Index, int32 RootBodyIndex, int32 ComponentIndex)
	{
		FUERLResetBinding Binding;
		Binding.Index = Index;
		Binding.Name = FName(*FString::Printf(TEXT("runtime.root_pose.%d"), ComponentIndex));
		Binding.TargetType = TEXT("root_pose");
		Binding.BodyIndex = RootBodyIndex;
		Binding.ComponentIndex = ComponentIndex;
		Binding.Unit = TEXT("m,quat_xyzw");
		return Binding;
	}
}

class FUERLSkeletalMeshRobotRuntime::FImpl
{
public:
	TUniquePtr<IUERLRobot> Robot;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<USkeletalMeshComponent> ClaimedMesh;
	TWeakObjectPtr<AActor> GroundQueryIgnoreActor;
	FUERLResetBatch ReferenceReset;
	bool bClaimAuthoredActor = false;
	bool bFixedBase = false;
	int32 LastSampledSolverFrame = INDEX_NONE;
	FUERLBatchBinding Binding;
	TArray<FUERLFieldDescriptor> SelectedStateFields;
	TArray<float> State;
	TArray<float> Actions;
	int32 StateWidth = 0;
	int32 ActuatorCount = 0;
};

FUERLSkeletalMeshRobotRuntime::FUERLSkeletalMeshRobotRuntime()
	: Impl(MakeUnique<FImpl>())
{
}

FUERLSkeletalMeshRobotRuntime::~FUERLSkeletalMeshRobotRuntime()
{
	Reset();
}

bool FUERLSkeletalMeshRobotRuntime::Initialize(
	UWorld& World,
	const FUERLSkeletalMeshRobotRuntimeConfig& Config,
	FString& OutError)
{
	Reset();
	if (Config.AssetPath.IsEmpty() || Config.Actuators.IsEmpty() || Config.Observations.IsEmpty()
		|| !Config.GroundNormal.IsNormalized() || !FMath::IsFinite(Config.InitialRootHeightMeters))
	{
		OutError = TEXT("in-game SkeletalMesh robot configuration is incomplete");
		return false;
	}

	const TSharedRef<IUERLRobotFactory> Factory = UERLGenericRobot::MakeFactory();
	FUERLProviderConfig ProviderConfig;
	ProviderConfig.AssetPath = Config.AssetPath;
	ProviderConfig.ClaimedMesh = Config.ClaimedMesh;
	ProviderConfig.PlacementTransform = Config.PlacementTransform;
	ProviderConfig.bHasPlacementTransform = Config.bHasPlacementTransform;
	ProviderConfig.Scalars.Add(UERLGenericRobot::ClaimAuthoredActor, Config.bClaimAuthoredActor ? 1.0 : 0.0);
	// Deployment runs several synchronous Chaos substeps per game frame.
	ProviderConfig.Scalars.Add(UERLGenericRobot::SolverStepCommands, 1.0);

	FUERLProviderConfig ReflectionConfig;
	if (!Factory->ValidateConfig(ProviderConfig, ReflectionConfig, OutError))
	{
		return false;
	}
	const FUERLRobotTopology& Topology = Factory->Describe().Topology;

	ProviderConfig.ResetBindings.Reserve((Topology.bFixedBase ? 0 : 7) + Config.Actuators.Num());
	if (!Topology.bFixedBase)
	{
		for (int32 ComponentIndex = 0; ComponentIndex < 7; ++ComponentIndex)
		{
			ProviderConfig.ResetBindings.Add(RootResetBinding(
				ProviderConfig.ResetBindings.Num(), Topology.RootBodyIndex, ComponentIndex));
		}
	}
	ProviderConfig.Actuators.Reserve(Config.Actuators.Num());
	for (int32 ActuatorIndex = 0; ActuatorIndex < Config.Actuators.Num(); ++ActuatorIndex)
	{
		const FUERLSkeletalMeshRuntimeActuator& Source = Config.Actuators[ActuatorIndex];
		int32 JointIndex = INDEX_NONE;
		const FUERLJointTopology* Joint = FindJoint(Topology, Source.JointName, JointIndex);
		if (!Joint || Joint->DegreesOfFreedom != 1)
		{
			OutError = FString::Printf(TEXT("in-game robot has no scalar joint '%s'"), *Source.JointName.ToString());
			return false;
		}
		FUERLActuatorConfig& Actuator = ProviderConfig.Actuators.AddDefaulted_GetRef();
		Actuator.Index = ActuatorIndex;
		Actuator.JointIndex = JointIndex;
		Actuator.JointName = Source.JointName;
		Actuator.CoordinateType = Joint->CoordinateType == EUERLJointCoordinateType::Revolute
			? TEXT("revolute") : TEXT("prismatic");
		Actuator.Coordinate = UERLJointCoordinateName(Joint->Coordinate);
		const bool bPositionTarget = Source.Stiffness > 0.0;
		Actuator.TargetMode = bPositionTarget ? TEXT("position") : TEXT("effort");
		Actuator.Unit = bPositionTarget
			? UERLJointCoordinateUnit(Joint->CoordinateType)
			: Joint->CoordinateType == EUERLJointCoordinateType::Prismatic ? TEXT("N") : TEXT("N*m");
		Actuator.Stiffness = Source.Stiffness;
		Actuator.Damping = Source.Damping;
		Actuator.EffortLimit = Source.EffortLimit;
		Actuator.DefaultPosition = Source.DefaultPosition;

		FUERLResetBinding& ResetBinding = ProviderConfig.ResetBindings.AddDefaulted_GetRef();
		ResetBinding.Index = ProviderConfig.ResetBindings.Num() - 1;
		ResetBinding.Name = FName(*FString::Printf(TEXT("runtime.joint.%s"), *Source.JointName.ToString()));
		ResetBinding.TargetType = TEXT("joint_position");
		ResetBinding.JointIndex = JointIndex;
		ResetBinding.BodyIndex = Joint->ChildBodyIndex;
		ResetBinding.Coordinate = UERLJointCoordinateName(Joint->Coordinate);
		ResetBinding.Unit = UERLJointCoordinateUnit(Joint->CoordinateType);
	}

	FUERLProviderConfig EffectiveConfig;
	if (!Factory->ValidateConfig(ProviderConfig, EffectiveConfig, OutError))
	{
		return false;
	}
	const FUERLRobotDescriptor& Descriptor = Factory->Describe();
	FUERLBatchSchema Schema;
	Schema.ActionFields.Add({ Descriptor.ActionFields[0], 0 });
	Schema.ActionWidth = Descriptor.ActionFields[0].Width;
	TArray<FUERLFieldDescriptor> SelectedStateFields;
	for (const FUERLSkeletalMeshRuntimeObservation& Request : Config.Observations)
	{
		const FUERLFieldDescriptor* Field = FindObservation(Descriptor.StateFields, Descriptor.Topology, Request);
		if (!Field)
		{
			OutError = FString::Printf(
				TEXT("in-game robot observation '%s' for '%s' is unavailable"),
				UERLObservationTypeName(Request.Type), *Request.TargetName.ToString());
			return false;
		}
		SelectedStateFields.Add(*Field);
		Schema.StateFields.Add({ *Field, Schema.StateWidth });
		Schema.StateWidth += Field->Width;
	}
	if (!Impl->Binding.Compile(Schema, Descriptor.ActionFields, Descriptor.StateFields, OutError))
	{
		return false;
	}

	Impl->Robot = Factory->Create(EffectiveConfig);
	if (!Impl->Robot->PrepareState(SelectedStateFields, OutError))
	{
		Reset();
		return false;
	}
	FUERLSlotCollisionPlan CollisionPlan;
	if (!CollisionPlan.Compile(1, EUERLEnvironmentCollisionScope::SharedWorld, OutError))
	{
		Reset();
		return false;
	}
	FUERLSlotContext Slot;
	Slot.SlotId = 0;
	Slot.Origin = Config.GroundOrigin;
	Slot.GroundHeight = Config.GroundOrigin.Z;
	Slot.GroundNormal = Config.GroundNormal;
	Slot.TerrainQueryActors = Config.TerrainQueryActors;
	Slot.TerrainQueryPurpose = EUERLTerrainQueryPurpose::DeploymentWorldStatic;
	Slot.CollisionProfile = CollisionPlan.Profile(0);
	if (!Impl->Robot->SpawnIntoSlots(World, { Slot }, OutError))
	{
		Reset();
		return false;
	}

	FUERLResetRow ResetRow;
	ResetRow.SlotId = 0;
	ResetRow.Origin = Config.GroundOrigin;
	ResetRow.GroundHeight = Config.GroundOrigin.Z;
	ResetRow.GroundNormal = Config.GroundNormal;
	if (!Topology.bFixedBase)
	{
		ResetRow.Values = {
			0.0, 0.0, Config.InitialRootHeightMeters,
			0.0, 0.0, 0.0, 1.0,
		};
	}
	for (const FUERLSkeletalMeshRuntimeActuator& Actuator : Config.Actuators)
	{
		ResetRow.Values.Add(Actuator.DefaultPosition);
	}
	FUERLResetBatch ResetBatch;
	ResetBatch.Rows.Add(MoveTemp(ResetRow));
	if (!Impl->Robot->ResetSlots(ResetBatch, OutError))
	{
		Reset();
		return false;
	}
	Impl->World = &World;
	Impl->ClaimedMesh = Config.ClaimedMesh;
	Impl->GroundQueryIgnoreActor = Config.GroundQueryIgnoreActor;
	Impl->bClaimAuthoredActor = Config.bClaimAuthoredActor;
	Impl->bFixedBase = Topology.bFixedBase;
	Impl->ReferenceReset = ResetBatch;
	FUERLSolverClockSnapshot Clock;
	FString ClockError;
	if (ReadUERLSolverClock(World, Clock, ClockError))
	{
		Impl->LastSampledSolverFrame = Clock.Frame;
	}

	Impl->StateWidth = Schema.StateWidth;
	Impl->ActuatorCount = Schema.ActionWidth;
	Impl->SelectedStateFields = MoveTemp(SelectedStateFields);
	Impl->State.SetNumZeroed(Impl->StateWidth);
	Impl->Actions.SetNumZeroed(Impl->ActuatorCount);
	return true;
}

void FUERLSkeletalMeshRobotRuntime::SamplePhysicsContacts(double SolverStepSeconds)
{
	if (Impl->Robot && FMath::IsFinite(SolverStepSeconds) && SolverStepSeconds > 0.0)
	{
		Impl->Robot->SamplePhysicsContacts(SolverStepSeconds);
	}
}

double FUERLSkeletalMeshRobotRuntime::GetLastSolverStepSeconds()
{
	FUERLSolverClockSnapshot Clock;
	FString Error;
	if (!Impl->World.IsValid() || !ReadUERLSolverClock(*Impl->World.Get(), Clock, Error))
	{
		return 0.0;
	}
	if (Clock.Frame <= Impl->LastSampledSolverFrame)
	{
		return 0.0;
	}
	Impl->LastSampledSolverFrame = Clock.Frame;
	const double SolverStepSeconds = Clock.LastDt;
	return FMath::IsFinite(SolverStepSeconds) && SolverStepSeconds > 0.0
		? SolverStepSeconds : 0.0;
}

bool FUERLSkeletalMeshRobotRuntime::ReadCompletedSolverClock(
	FUERLSolverClockSnapshot& OutClock,
	FString& OutError) const
{
	if (!Impl->World.IsValid())
	{
		OutClock = FUERLSolverClockSnapshot();
		OutError = TEXT("in-game SkeletalMesh robot has no active World");
		return false;
	}
	return ReadUERLSolverClock(*Impl->World.Get(), OutClock, OutError);
}

void FUERLSkeletalMeshRobotRuntime::ClearContactState()
{
	if (Impl->Robot)
	{
		Impl->Robot->ClearContactState();
	}
}

void FUERLSkeletalMeshRobotRuntime::ClearObservationCaches()
{
	if (Impl->Robot)
	{
		Impl->Robot->ClearObservationCaches();
	}
}

bool FUERLSkeletalMeshRobotRuntime::CollectState(TArray<float>& OutState, FString& OutError) const
{
	if (!Impl->Robot || Impl->StateWidth <= 0)
	{
		OutError = TEXT("in-game SkeletalMesh robot is not initialized");
		return false;
	}
	FUERLNamedStateWriter Writer(
		Impl->Binding,
		{ Impl->State.GetData(), 1, Impl->StateWidth });
	if (!Impl->Robot->TryCollectState({ 0 }, Writer, OutError))
	{
		return false;
	}
	OutState = Impl->State;
	return true;
}

bool FUERLSkeletalMeshRobotRuntime::GetPrimaryActorTransform(FTransform& OutTransform, FString& OutError) const
{
	if (!Impl->Robot)
	{
		OutTransform = FTransform::Identity;
		OutError = TEXT("in-game SkeletalMesh robot is not initialized");
		return false;
	}
	return Impl->Robot->GetPrimaryActorTransform(OutTransform, OutError);
}

USkeletalMeshComponent* FUERLSkeletalMeshRobotRuntime::GetControlledMeshComponent() const
{
	// The spawned robot owns a separate actor whose root body already drives its
	// component (SimulationUpatesComponentTransform), so only the claimed mesh
	// needs an explicit sync target.
	return Impl && Impl->ClaimedMesh.IsValid() ? Impl->ClaimedMesh.Get() : nullptr;
}

bool FUERLSkeletalMeshRobotRuntime::ApplyActuatorTargets(
	TConstArrayView<float> Targets,
	FString& OutError)
{
	if (!Impl->Robot || Targets.Num() != Impl->ActuatorCount)
	{
		OutError = TEXT("in-game SkeletalMesh robot target width does not match its actuators");
		return false;
	}
	Impl->Actions.SetNumUninitialized(Targets.Num());
	FMemory::Memcpy(Impl->Actions.GetData(), Targets.GetData(), Targets.Num() * sizeof(float));
	const FUERLNamedActionReader Reader(
		Impl->Binding,
		{ Impl->Actions.GetData(), 1, Impl->ActuatorCount });
	return Impl->Robot->ApplyCommands(Reader, OutError);
}

bool FUERLSkeletalMeshRobotRuntime::ResetToReferencePose(FString& OutError)
{
	OutError.Reset();
	if (!Impl->Robot || Impl->ReferenceReset.Rows.IsEmpty())
	{
		OutError = TEXT("in-game SkeletalMesh robot has no captured reference pose");
		return false;
	}
	FUERLResetBatch ResetBatch = Impl->ReferenceReset;
	if (!Impl->bFixedBase && Impl->World.IsValid())
	{
		FTransform CurrentTransform;
		if (!Impl->Robot->GetPrimaryActorTransform(CurrentTransform, OutError))
		{
			return false;
		}
		// Start just above the root like StartPolicy so a ceiling cannot become the ground.
		const FVector TraceStart = CurrentTransform.GetLocation() + FVector::UpVector * 10.0;
		const FVector TraceEnd = CurrentTransform.GetLocation() - FVector::UpVector * 10000.0;
		FCollisionQueryParams QueryParams(FCollisionQueryParams::DefaultQueryParam);
		QueryParams.bTraceComplex = false;
		AActor* GroundQueryIgnoreActor = Impl->GroundQueryIgnoreActor.Get();
		if (!GroundQueryIgnoreActor && Impl->ClaimedMesh.IsValid())
		{
			GroundQueryIgnoreActor = Impl->ClaimedMesh->GetOwner();
		}
		if (GroundQueryIgnoreActor)
		{
			QueryParams.AddIgnoredActor(GroundQueryIgnoreActor);
		}
		FHitResult GroundHit;
		if (!Impl->World->LineTraceSingleByObjectType(
			GroundHit,
			TraceStart,
			TraceEnd,
			FCollisionObjectQueryParams(ECC_WorldStatic),
			QueryParams)
			|| !GroundHit.bBlockingHit)
		{
			OutError = TEXT("reference-pose reset found no valid WorldStatic ground at the current root XY");
			return false;
		}
		const FQuat AlignToGround = FQuat::FindBetweenNormals(
			FVector::UpVector, GroundHit.ImpactNormal.GetSafeNormal());
		const FQuat YawAroundGround(
			GroundHit.ImpactNormal.GetSafeNormal(),
			FMath::DegreesToRadians(CurrentTransform.Rotator().Yaw));
		const FQuat DesiredWorldRotation = YawAroundGround * AlignToGround;
		FUERLResetRow& Row = ResetBatch.Rows[0];
		if (Impl->bClaimAuthoredActor)
		{
			// A claimed Owner transform must not move (architecture §10.3), so
			// express the desired world pose in the Owner slot frame instead.
			const AActor* Owner = Impl->ClaimedMesh.IsValid() ? Impl->ClaimedMesh->GetOwner() : nullptr;
			if (!Owner)
			{
				OutError = TEXT("reference-pose reset lost the claimed robot owner");
				return false;
			}
			const FTransform OwnerTransform = Owner->GetActorTransform();
			const FQuat OwnerFrameRotation =
				FRotator(0.0f, OwnerTransform.Rotator().Yaw, 0.0f).Quaternion();
			const FVector DesiredWorldLocation =
				FVector(CurrentTransform.GetLocation().X, CurrentTransform.GetLocation().Y, GroundHit.ImpactPoint.Z)
				+ DesiredWorldRotation.RotateVector(FVector(0.0, 0.0, Row.Values[2] * 100.0));
			const FVector LocalPosition = OwnerFrameRotation.Inverse().RotateVector(
				DesiredWorldLocation - OwnerTransform.GetLocation());
			const FQuat LocalRotation = (OwnerFrameRotation.Inverse() * DesiredWorldRotation).GetNormalized();
			Row.Values[0] = LocalPosition.X / 100.0;
			Row.Values[1] = LocalPosition.Y / 100.0;
			Row.Values[2] = LocalPosition.Z / 100.0;
			Row.Values[3] = LocalRotation.X;
			Row.Values[4] = LocalRotation.Y;
			Row.Values[5] = LocalRotation.Z;
			Row.Values[6] = LocalRotation.W;
		}
		else
		{
			Row.Origin = FVector(CurrentTransform.GetLocation().X, CurrentTransform.GetLocation().Y, GroundHit.ImpactPoint.Z);
			Row.GroundHeight = GroundHit.ImpactPoint.Z;
			Row.GroundNormal = GroundHit.ImpactNormal.GetSafeNormal();
			Row.GroundRotation = DesiredWorldRotation;
			Row.bHasGroundRotation = true;
		}
	}
	if (!Impl->Robot->ResetSlots(ResetBatch, OutError))
	{
		return false;
	}
	Impl->Robot->ClearObservationCaches();
	return true;
}

void FUERLSkeletalMeshRobotRuntime::Reset()
{
	if (Impl && Impl->Robot)
	{
		Impl->Robot->DestroySlots();
	}
	if (Impl)
	{
		Impl->Robot.Reset();
		Impl->World.Reset();
		Impl->ClaimedMesh.Reset();
		Impl->ReferenceReset = FUERLResetBatch();
		Impl->bClaimAuthoredActor = false;
		Impl->bFixedBase = false;
		Impl->LastSampledSolverFrame = INDEX_NONE;
		Impl->Binding.Reset();
		Impl->SelectedStateFields.Reset();
		Impl->State.Reset();
		Impl->Actions.Reset();
		Impl->StateWidth = 0;
		Impl->ActuatorCount = 0;
	}
}

int32 FUERLSkeletalMeshRobotRuntime::StateWidth() const
{
	return Impl->StateWidth;
}

int32 FUERLSkeletalMeshRobotRuntime::ActuatorCount() const
{
	return Impl->ActuatorCount;
}

bool FUERLSkeletalMeshRobotRuntime::IsInitialized() const
{
	return Impl->Robot.IsValid();
}

const TArray<FUERLFieldDescriptor>& FUERLSkeletalMeshRobotRuntime::SelectedStateFields() const
{
	return Impl->SelectedStateFields;
}

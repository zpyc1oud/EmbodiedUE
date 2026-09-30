#include "UERLGenericRobotResetApplier.h"

#include "UERLGenericRobotKinematics.h"
#include "UERLGenericRobotResetJoints.h"

#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"

namespace
{
	bool SetRootReset(
		FUERLRobotResetSlotRef& Slot,
		const FUERLRobotTopology& Topology,
		const TArray<double>* RootPoseValues,
		const TArray<double>* RootVelocityValues,
		bool bApply,
		FString& OutError)
	{
		checkf(Slot.Component && Slot.BoneIndices && Slot.Origin && Slot.GroundRotation
			&& Slot.CanonicalBodies, TEXT("generic Robot root reset Slot view is incomplete"));
		if (!Topology.BodyNames.IsValidIndex(Topology.RootBodyIndex)
			|| !Slot.BoneIndices->IsValidIndex(Topology.RootBodyIndex))
		{
			OutError = TEXT("generic Robot root reset has no valid root body");
			return false;
		}
		const FName RootName = Topology.BodyNames[Topology.RootBodyIndex];
		FBodyInstance* RootInstance = Slot.Component->GetBodyInstance(RootName);
		if (!RootInstance)
		{
			OutError = FString::Printf(
				TEXT("generic Robot root reset has no body instance for '%s'"), *RootName.ToString());
			return false;
		}

		FUERLGenericBodyState CurrentRoot;
		CurrentRoot.Transform = RootInstance->GetUnrealWorldTransform();
		CurrentRoot.LinearVelocity = RootInstance->GetUnrealWorldVelocity();
		CurrentRoot.AngularVelocity = RootInstance->GetUnrealWorldAngularVelocityInRadians();
		FUERLGenericBodyState TargetRoot = CurrentRoot;
		FTransform RobotRootWorld;
		bool bResetRobotRootTransform = false;
		if (RootPoseValues)
		{
			const TArray<double>& Values = *RootPoseValues;
			const FQuat LocalRotation(
				static_cast<float>(Values[3]),
				static_cast<float>(Values[4]),
				static_cast<float>(Values[5]),
				static_cast<float>(Values[6]));
			if (FMath::Abs(LocalRotation.SizeSquared() - 1.0f) > 1.0e-4f)
			{
				OutError = TEXT("generic Robot root_pose quaternion must be normalized");
				return false;
			}
			const FVector LocalPosition(
				static_cast<float>(Values[0] * 100.0),
				static_cast<float>(Values[1] * 100.0),
				static_cast<float>(Values[2] * 100.0));
			RobotRootWorld.SetLocation(*Slot.Origin + Slot.GroundRotation->RotateVector(LocalPosition));
			RobotRootWorld.SetRotation((*Slot.GroundRotation * LocalRotation).GetNormalized());
			bResetRobotRootTransform = true;
			checkf(
				Slot.CanonicalBodies->IsValidIndex(Topology.RootBodyIndex),
				TEXT("generic Robot root reset has no canonical reference transform"));
			TargetRoot.Transform = ComposeGenericRootBodyWorld(
				(*Slot.CanonicalBodies)[Topology.RootBodyIndex].SlotTransform,
				RobotRootWorld);
		}
		if (RootVelocityValues)
		{
			const TArray<double>& Values = *RootVelocityValues;
			const FVector LocalLinear(
				static_cast<float>(Values[0] * 100.0),
				static_cast<float>(Values[1] * 100.0),
				static_cast<float>(Values[2] * 100.0));
			const FVector LocalAngular(
				static_cast<float>(Values[3]),
				static_cast<float>(Values[4]),
				static_cast<float>(Values[5]));
			TargetRoot.LinearVelocity = Slot.GroundRotation->RotateVector(LocalLinear);
			TargetRoot.AngularVelocity = Slot.GroundRotation->RotateVector(LocalAngular);
		}
		TArray<FUERLGenericBodyState> RebasedBodies;
		RebasedBodies.Reserve(Topology.BodyNames.Num());
		for (const FName BodyName : Topology.BodyNames)
		{
			FBodyInstance* BodyInstance = Slot.Component->GetBodyInstance(BodyName);
			if (!BodyInstance)
			{
				OutError = FString::Printf(
					TEXT("generic Robot root reset has no body instance for '%s'"), *BodyName.ToString());
				return false;
			}
			FUERLGenericBodyState Body;
			Body.Transform = BodyInstance->GetUnrealWorldTransform();
			Body.LinearVelocity = BodyInstance->GetUnrealWorldVelocity();
			Body.AngularVelocity = BodyInstance->GetUnrealWorldAngularVelocityInRadians();
			RebasedBodies.Add(RebaseGenericBodyState(Body, CurrentRoot, TargetRoot));
		}
		if (!bApply)
		{
			return true;
		}
		if (bResetRobotRootTransform)
		{
			Slot.Component->SetWorldLocationAndRotation(
				RobotRootWorld.GetLocation(),
				RobotRootWorld.GetRotation().Rotator(),
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
		}
		for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
		{
			FBodyInstance* BodyInstance = Slot.Component->GetBodyInstance(Topology.BodyNames[BodyIndex]);
			const FUERLGenericBodyState& Rebased = RebasedBodies[BodyIndex];
			BodyInstance->SetBodyTransform(Rebased.Transform, ETeleportType::TeleportPhysics, false);
			BodyInstance->SetLinearVelocity(Rebased.LinearVelocity, false);
			BodyInstance->SetAngularVelocityInRadians(Rebased.AngularVelocity, false);
		}
		return true;
	}

	bool CollectRootOverride(
		TConstArrayView<FUERLResetBinding> ResetBindings,
		const FUERLResetRow& Row,
		TArray<double>& OutRootPoseValues,
		TArray<bool>& OutRootPoseSet,
		TArray<double>& OutRootVelocityValues,
		TArray<bool>& OutRootVelocitySet,
		bool bValidateJoints,
		FUERLRobotResetSlotRef* Slot,
		const FUERLRobotTopology* Topology,
		TConstArrayView<int32> ParentBodyIndices,
		FString& OutError)
	{
		OutRootPoseValues.Reset();
		OutRootPoseSet.Reset();
		OutRootVelocityValues.Reset();
		OutRootVelocitySet.Reset();
		for (int32 BindingIndex = 0; BindingIndex < ResetBindings.Num(); ++BindingIndex)
		{
			const FUERLResetBinding& Binding = ResetBindings[BindingIndex];
			if (!Row.Values.IsValidIndex(Binding.Index) || !FMath::IsFinite(Row.Values[Binding.Index]))
			{
				OutError = FString::Printf(
					TEXT("generic Robot reset is missing finite value '%s'"), *Binding.Name.ToString());
				return false;
			}
			const double Value = Row.Values[Binding.Index];
			if (Binding.TargetType == TEXT("root_pose"))
			{
				if (!OutRootPoseValues.Num())
				{
					OutRootPoseValues.Init(0.0, 7);
					OutRootPoseSet.Init(false, 7);
				}
				if (!OutRootPoseValues.IsValidIndex(Binding.ComponentIndex)
					|| OutRootPoseSet[Binding.ComponentIndex])
				{
					OutError = FString::Printf(
						TEXT("generic Robot root_pose component '%s' is duplicated or invalid"),
						*Binding.Name.ToString());
					return false;
				}
				OutRootPoseValues[Binding.ComponentIndex] = Value;
				OutRootPoseSet[Binding.ComponentIndex] = true;
			}
			else if (Binding.TargetType == TEXT("root_velocity"))
			{
				if (!OutRootVelocityValues.Num())
				{
					OutRootVelocityValues.Init(0.0, 6);
					OutRootVelocitySet.Init(false, 6);
				}
				if (!OutRootVelocityValues.IsValidIndex(Binding.ComponentIndex)
					|| OutRootVelocitySet[Binding.ComponentIndex])
				{
					OutError = FString::Printf(
						TEXT("generic Robot root_velocity component '%s' is duplicated or invalid"),
						*Binding.Name.ToString());
					return false;
				}
				OutRootVelocityValues[Binding.ComponentIndex] = Value;
				OutRootVelocitySet[Binding.ComponentIndex] = true;
			}
			else if (bValidateJoints
				&& (Binding.TargetType == TEXT("joint_position")
					|| Binding.TargetType == TEXT("joint_velocity")))
			{
				checkf(Slot && Topology, TEXT("joint reset validation requires Slot and Topology"));
				if (!SetGenericRobotJointReset(
					*Slot, *Topology, ParentBodyIndices, Binding, Value, OutError, false))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool RequireCompleteRootOverride(
		const TArray<double>& Values,
		const TArray<bool>& SetFlags,
		const TCHAR* IncompleteError,
		FString& OutError)
	{
		if (!Values.Num())
		{
			return true;
		}
		for (const bool bSet : SetFlags)
		{
			if (!bSet)
			{
				OutError = IncompleteError;
				return false;
			}
		}
		return true;
	}
}

bool ResetGenericRobotSlots(
	TArrayView<FUERLRobotResetSlotRef> Slots,
	const FUERLRobotTopology& Topology,
	TConstArrayView<int32> ParentBodyIndices,
	TConstArrayView<FUERLActuatorConfig> Actuators,
	TConstArrayView<FUERLResetBinding> ResetBindings,
	const FUERLResetBatch& Reset,
	FString& OutError)
{
	// Validate the complete batch before changing any body.  This keeps a
	// malformed override or missing runtime joint from partially committing
	// an earlier Slot reset.
	for (const FUERLResetRow& Row : Reset.Rows)
	{
		FUERLRobotResetSlotRef* Slot = Slots.IsValidIndex(Row.SlotId) ? &Slots[Row.SlotId] : nullptr;
		if (!Slot || !Slot->Owner || !Slot->Component)
		{
			OutError = FString::Printf(TEXT("missing generic Robot body for reset Slot %d"), Row.SlotId);
			return false;
		}
		checkf(
			Slot->CanonicalBodies && Slot->CanonicalBodies->Num() == Topology.BodyNames.Num(),
			TEXT("generic Robot canonical body state count does not match topology"));
		for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
		{
			checkf(
				Slot->Component->GetBodyInstance(Topology.BodyNames[BodyIndex]),
				TEXT("generic Robot reset body %d is missing"), BodyIndex);
		}
		if (Row.Values.IsEmpty())
		{
			continue;
		}
		if (Row.Values.Num() != ResetBindings.Num())
		{
			OutError = TEXT("generic Robot reset override vector does not match the negotiated bindings");
			return false;
		}
		TArray<double> RootPoseValues;
		TArray<bool> RootPoseSet;
		TArray<double> RootVelocityValues;
		TArray<bool> RootVelocitySet;
		if (!CollectRootOverride(
			ResetBindings, Row, RootPoseValues, RootPoseSet, RootVelocityValues, RootVelocitySet,
			true, Slot, &Topology, ParentBodyIndices, OutError))
		{
			return false;
		}
		if (!RequireCompleteRootOverride(
			RootPoseValues, RootPoseSet, TEXT("generic Robot root_pose reset is incomplete"), OutError)
			|| !RequireCompleteRootOverride(
				RootVelocityValues, RootVelocitySet,
				TEXT("generic Robot root_velocity reset is incomplete"), OutError))
		{
			return false;
		}
		if (RootPoseValues.Num()
			&& !SetRootReset(*Slot, Topology, &RootPoseValues, nullptr, false, OutError))
		{
			return false;
		}
		if (RootVelocityValues.Num()
			&& !SetRootReset(*Slot, Topology, nullptr, &RootVelocityValues, false, OutError))
		{
			return false;
		}
	}

	for (const FUERLResetRow& Row : Reset.Rows)
	{
		FUERLRobotResetSlotRef* Slot = Slots.IsValidIndex(Row.SlotId) ? &Slots[Row.SlotId] : nullptr;
		if (!Slot || !Slot->Owner || !Slot->Component || !Slot->Origin || !Slot->GroundRotation
			|| !Slot->CanonicalBodies || !Slot->Targets || !Slot->ContactState || !Slot->bTerrainBoundary)
		{
			OutError = FString::Printf(TEXT("missing generic Robot body for reset Slot %d"), Row.SlotId);
			return false;
		}
		if (Slot->bPreserveOwnerTransform)
		{
			const FTransform ExistingOwnerTransform = Slot->Owner->GetActorTransform();
			*Slot->Origin = ExistingOwnerTransform.GetLocation();
			*Slot->GroundRotation = FRotator(
				0.0f,
				ExistingOwnerTransform.Rotator().Yaw,
				0.0f).Quaternion();
		}
		else
		{
			*Slot->Origin = Row.Origin;
			*Slot->GroundRotation = Row.bHasGroundRotation
				? Row.GroundRotation
				: FQuat::FindBetweenNormals(FVector::UpVector, Row.GroundNormal.GetSafeNormal());
		}
		*Slot->bTerrainBoundary = false;
		if (!Slot->bPreserveOwnerTransform)
		{
			Slot->Owner->SetActorLocationAndRotation(
				*Slot->Origin,
				Slot->GroundRotation->Rotator(),
				false,
				nullptr,
				ETeleportType::TeleportPhysics);
		}
		const FTransform SlotFrame(*Slot->GroundRotation, *Slot->Origin);
		checkf(
			Slot->CanonicalBodies->Num() == Topology.BodyNames.Num(),
			TEXT("generic Robot canonical body state count does not match topology"));
		for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
		{
			FBodyInstance* Body = Slot->Component->GetBodyInstance(Topology.BodyNames[BodyIndex]);
			checkf(Body, TEXT("generic Robot reset body %d is missing"), BodyIndex);
			const FUERLGenericRobotCanonicalBody& Canonical = (*Slot->CanonicalBodies)[BodyIndex];
			Body->ClearForces();
			Body->ClearTorques();
			Body->SetBodyTransform(
				Canonical.SlotTransform * SlotFrame, ETeleportType::TeleportPhysics, false);
			Body->SetLinearVelocity(
				Slot->GroundRotation->RotateVector(Canonical.SlotLinearVelocity), false);
			Body->SetAngularVelocityInRadians(
				Slot->GroundRotation->RotateVector(Canonical.SlotAngularVelocity), false);
		}
		Slot->Targets->SetNum(Actuators.Num());
		for (int32 ActuatorIndex = 0; ActuatorIndex < Actuators.Num(); ++ActuatorIndex)
		{
			(*Slot->Targets)[ActuatorIndex] = Actuators[ActuatorIndex].TargetMode == TEXT("position")
				? static_cast<float>(Actuators[ActuatorIndex].DefaultPosition) : 0.0f;
		}
		if (!Row.Values.IsEmpty() && Row.Values.Num() != ResetBindings.Num())
		{
			OutError = TEXT("generic Robot reset override vector does not match the negotiated bindings");
			return false;
		}
		TArray<double> RootPoseValues;
		TArray<bool> RootPoseSet;
		TArray<double> RootVelocityValues;
		TArray<bool> RootVelocitySet;
		if (!Row.Values.IsEmpty()
			&& !CollectRootOverride(
				ResetBindings, Row, RootPoseValues, RootPoseSet, RootVelocityValues, RootVelocitySet,
				false, nullptr, nullptr, ParentBodyIndices, OutError))
		{
			return false;
		}
		if (!RequireCompleteRootOverride(
			RootPoseValues, RootPoseSet, TEXT("generic Robot root_pose reset is incomplete"), OutError)
			|| !RequireCompleteRootOverride(
				RootVelocityValues, RootVelocitySet,
				TEXT("generic Robot root_velocity reset is incomplete"), OutError))
		{
			return false;
		}
		if (RootPoseValues.Num()
			&& !SetRootReset(*Slot, Topology, &RootPoseValues, nullptr, true, OutError))
		{
			return false;
		}
		if (!ApplyGenericRobotJointResetBindings(
			*Slot, Topology, ParentBodyIndices, ResetBindings, Row, TEXT("joint_position"), OutError))
		{
			return false;
		}
		if (RootVelocityValues.Num()
			&& !SetRootReset(*Slot, Topology, nullptr, &RootVelocityValues, true, OutError))
		{
			return false;
		}
		if (!ApplyGenericRobotJointResetBindings(
			*Slot, Topology, ParentBodyIndices, ResetBindings, Row, TEXT("joint_velocity"), OutError))
		{
			return false;
		}
		Slot->Component->WakeAllRigidBodies();
		Slot->ContactState->Clear();
	}
	return true;
}

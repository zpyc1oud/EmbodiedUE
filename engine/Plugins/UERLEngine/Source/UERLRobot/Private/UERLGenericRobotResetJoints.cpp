#include "UERLGenericRobotResetJoints.h"

#include "UERLGenericRobotKinematics.h"

#include "Components/SkeletalMeshComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"

namespace
{
	bool IsBodyInSubtree(
		TConstArrayView<int32> ParentBodyIndices,
		int32 BodyIndex,
		int32 SubtreeRootBodyIndex)
	{
		for (int32 Current = BodyIndex; Current != INDEX_NONE; Current = ParentBodyIndices[Current])
		{
			if (Current == SubtreeRootBodyIndex)
			{
				return true;
			}
		}
		return false;
	}

	bool RebaseJointSubtree(
		FUERLRobotResetSlotRef& Slot,
		const FUERLRobotTopology& Topology,
		TConstArrayView<int32> ParentBodyIndices,
		int32 SubtreeRootBodyIndex,
		const FUERLGenericBodyState& TargetRoot,
		FString& OutError)
	{
		checkf(Slot.Component, TEXT("generic Robot joint subtree has no component"));
		FBodyInstance* RootInstance = Slot.Component->GetBodyInstance(
			Topology.BodyNames[SubtreeRootBodyIndex]);
		if (!RootInstance)
		{
			OutError = TEXT("generic Robot joint subtree root has no runtime body instance");
			return false;
		}
		FUERLGenericBodyState CurrentRoot;
		CurrentRoot.Transform = RootInstance->GetUnrealWorldTransform();
		CurrentRoot.LinearVelocity = RootInstance->GetUnrealWorldVelocity();
		CurrentRoot.AngularVelocity = RootInstance->GetUnrealWorldAngularVelocityInRadians();

		TArray<FUERLGenericBodyState> RebasedBodies;
		RebasedBodies.SetNum(Topology.BodyNames.Num());
		for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
		{
			if (!IsBodyInSubtree(ParentBodyIndices, BodyIndex, SubtreeRootBodyIndex))
			{
				continue;
			}
			FBodyInstance* BodyInstance = Slot.Component->GetBodyInstance(Topology.BodyNames[BodyIndex]);
			if (!BodyInstance)
			{
				OutError = TEXT("generic Robot joint subtree has no runtime body instance");
				return false;
			}
			FUERLGenericBodyState Body;
			Body.Transform = BodyInstance->GetUnrealWorldTransform();
			Body.LinearVelocity = BodyInstance->GetUnrealWorldVelocity();
			Body.AngularVelocity = BodyInstance->GetUnrealWorldAngularVelocityInRadians();
			RebasedBodies[BodyIndex] = RebaseGenericBodyState(Body, CurrentRoot, TargetRoot);
		}
		for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
		{
			if (!IsBodyInSubtree(ParentBodyIndices, BodyIndex, SubtreeRootBodyIndex))
			{
				continue;
			}
			FBodyInstance* BodyInstance = Slot.Component->GetBodyInstance(Topology.BodyNames[BodyIndex]);
			const FUERLGenericBodyState& Body = RebasedBodies[BodyIndex];
			BodyInstance->SetBodyTransform(Body.Transform, ETeleportType::TeleportPhysics, false);
			BodyInstance->SetLinearVelocity(Body.LinearVelocity, false);
			BodyInstance->SetAngularVelocityInRadians(Body.AngularVelocity, false);
		}
		return true;
	}
}

bool SetGenericRobotJointReset(
	FUERLRobotResetSlotRef& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<int32> ParentBodyIndices,
	const FUERLResetBinding& Binding,
	double Value,
	FString& OutError,
	bool bApply)
{
	checkf(Slot.Component && Slot.ConstraintIndices, TEXT("generic Robot joint reset Slot view is incomplete"));
	if (!Topology.Joints.IsValidIndex(Binding.JointIndex)
		|| !Slot.ConstraintIndices->IsValidIndex(Binding.JointIndex))
	{
		OutError = FString::Printf(
			TEXT("generic Robot reset '%s' has an invalid joint index"), *Binding.Name.ToString());
		return false;
	}
	const FUERLJointTopology& Joint = Topology.Joints[Binding.JointIndex];
	if (Binding.TargetType == TEXT("joint_position") && Joint.bHasPositionLimit
		&& (Value < Joint.LowerLimit || Value > Joint.UpperLimit))
	{
		OutError = FString::Printf(
			TEXT("generic Robot reset '%s' is outside the reflected joint limit"), *Binding.Name.ToString());
		return false;
	}
	FConstraintInstance* Constraint = Slot.Component->GetConstraintInstanceByIndex(
		(*Slot.ConstraintIndices)[Binding.JointIndex]);
	if (!Constraint || !Topology.BodyNames.IsValidIndex(Joint.ChildBodyIndex)
		|| !Topology.BodyNames.IsValidIndex(Joint.ParentBodyIndex))
	{
		OutError = FString::Printf(
			TEXT("generic Robot reset '%s' has no valid runtime joint"), *Binding.Name.ToString());
		return false;
	}
	const FName ChildName = Topology.BodyNames[Joint.ChildBodyIndex];
	const FName ParentName = Topology.BodyNames[Joint.ParentBodyIndex];
	FBodyInstance* ChildInstance = Slot.Component->GetBodyInstance(ChildName);
	FBodyInstance* ParentInstance = Slot.Component->GetBodyInstance(ParentName);
	if (!ChildInstance || !ParentInstance)
	{
		OutError = FString::Printf(
			TEXT("generic Robot reset '%s' has no runtime body instance"),
			*Binding.Name.ToString());
		return false;
	}
	const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
		Constraint->GetRefFrame(EConstraintFrame::Frame1),
		ChildInstance->GetUnrealWorldTransform());
	const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
		Constraint->GetRefFrame(EConstraintFrame::Frame2),
		ParentInstance->GetUnrealWorldTransform());
	const FVector AxisWorld = ChildFrame.TransformVectorNoScale(
		GenericJointCoordinateAxis(Joint.Coordinate)).GetSafeNormal();
	if (AxisWorld.IsNearlyZero())
	{
		OutError = FString::Printf(
			TEXT("generic Robot reset '%s' has a zero constraint axis"), *Binding.Name.ToString());
		return false;
	}

	if (Binding.TargetType == TEXT("joint_position"))
	{
		FTransform TargetFrame = ParentFrame;
		if (Joint.CoordinateType == EUERLJointCoordinateType::Prismatic)
		{
			TargetFrame.AddToTranslation(AxisWorld * static_cast<float>(Value * 100.0));
		}
		else if (Joint.CoordinateType == EUERLJointCoordinateType::Revolute)
		{
			const FTransform CoordinateRotation(
				FQuat(GenericJointCoordinateAxis(Joint.Coordinate), static_cast<float>(Value)));
			TargetFrame = CoordinateRotation * ParentFrame;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("generic Robot reset '%s' has an unknown coordinate type"), *Binding.Name.ToString());
			return false;
		}
		const FTransform TargetBody = RecoverGenericBodyWorldFromConstraintFrame(
			Constraint->GetRefFrame(EConstraintFrame::Frame1),
			TargetFrame);
		FUERLGenericBodyState TargetChild;
		TargetChild.Transform = TargetBody;
		TargetChild.LinearVelocity = ChildInstance->GetUnrealWorldVelocity();
		TargetChild.AngularVelocity = ChildInstance->GetUnrealWorldAngularVelocityInRadians();
		return bApply
			? RebaseJointSubtree(
				Slot, Topology, ParentBodyIndices, Joint.ChildBodyIndex, TargetChild, OutError)
			: true;
	}

	if (Binding.TargetType == TEXT("joint_velocity"))
	{
		FUERLGenericBodyState TargetChild;
		TargetChild.Transform = ChildInstance->GetUnrealWorldTransform();
		const FVector Anchor = (ChildFrame.GetLocation() + ParentFrame.GetLocation()) * 0.5;
		if (Joint.CoordinateType == EUERLJointCoordinateType::Revolute)
		{
			TargetChild.AngularVelocity = ParentInstance->GetUnrealWorldAngularVelocityInRadians()
				+ AxisWorld * static_cast<float>(Value);
			TargetChild.LinearVelocity = ParentInstance->GetUnrealWorldVelocityAtPoint(Anchor)
				+ FVector::CrossProduct(
					TargetChild.AngularVelocity,
					TargetChild.Transform.GetLocation() - Anchor);
		}
		else
		{
			TargetChild.AngularVelocity = ParentInstance->GetUnrealWorldAngularVelocityInRadians();
			const FVector AnchorVelocity = ParentInstance->GetUnrealWorldVelocityAtPoint(Anchor)
				+ AxisWorld * static_cast<float>(Value * 100.0);
			TargetChild.LinearVelocity = AnchorVelocity
				+ FVector::CrossProduct(
					TargetChild.AngularVelocity,
					TargetChild.Transform.GetLocation() - Anchor);
		}
		return bApply
			? RebaseJointSubtree(
				Slot, Topology, ParentBodyIndices, Joint.ChildBodyIndex, TargetChild, OutError)
			: true;
	}
	OutError = FString::Printf(
		TEXT("generic Robot reset '%s' has an unknown target type"), *Binding.Name.ToString());
	return false;
}

bool ApplyGenericRobotJointResetBindings(
	FUERLRobotResetSlotRef& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<int32> ParentBodyIndices,
	TConstArrayView<FUERLResetBinding> ResetBindings,
	const FUERLResetRow& Row,
	const FString& TargetType,
	FString& OutError)
{
	if (Row.Values.IsEmpty())
	{
		return true;
	}
	for (int32 JointIndex = 0; JointIndex < Topology.Joints.Num(); ++JointIndex)
	{
		for (const FUERLResetBinding& Binding : ResetBindings)
		{
			if (Binding.TargetType == TargetType && Binding.JointIndex == JointIndex
				&& !SetGenericRobotJointReset(
					Slot, Topology, ParentBodyIndices, Binding, Row.Values[Binding.Index], OutError, true))
			{
				return false;
			}
		}
	}
	return true;
}

#include "UERLTopologyReflector.h"
#include "UERLGenericRobotKinematics.h"
#include "UERLRobotLog.h"
#include "UERLTopologyReflectionInternal.h"

#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

namespace
{
	void AppendLinearCoordinate(
		ELinearConstraintMotion Motion,
		float LimitCentimetres,
		EUERLJointCoordinate Coordinate,
		FUERLJointTopology& Joint)
	{
		if (Motion == ELinearConstraintMotion::LCM_Locked)
		{
			return;
		}

		++Joint.DegreesOfFreedom;
		if (Joint.DegreesOfFreedom != 1)
		{
			return;
		}

		Joint.Coordinate = Coordinate;
		Joint.CoordinateType = EUERLJointCoordinateType::Prismatic;
		if (Motion == ELinearConstraintMotion::LCM_Limited)
		{
			const double LimitMetres = static_cast<double>(LimitCentimetres) / 100.0;
			Joint.bHasPositionLimit = true;
			Joint.LowerLimit = -LimitMetres;
			Joint.UpperLimit = LimitMetres;
		}
	}

	void AppendAngularCoordinate(
		EAngularConstraintMotion Motion,
		float LimitDegrees,
		EUERLJointCoordinate Coordinate,
		FUERLJointTopology& Joint)
	{
		if (Motion == EAngularConstraintMotion::ACM_Locked)
		{
			return;
		}

		++Joint.DegreesOfFreedom;
		if (Joint.DegreesOfFreedom != 1)
		{
			return;
		}

		Joint.Coordinate = Coordinate;
		Joint.CoordinateType = EUERLJointCoordinateType::Revolute;
		if (Motion == EAngularConstraintMotion::ACM_Limited)
		{
			const double LimitRadians = FMath::DegreesToRadians(static_cast<double>(LimitDegrees));
			Joint.bHasPositionLimit = true;
			Joint.LowerLimit = -LimitRadians;
			Joint.UpperLimit = LimitRadians;
		}
	}

	void ReflectConstraintCoordinates(const FConstraintInstance& Instance, FUERLJointTopology& Joint)
	{
		AppendLinearCoordinate(
			Instance.GetLinearXMotion(), Instance.GetLinearLimit(), EUERLJointCoordinate::LinearX, Joint);
		AppendLinearCoordinate(
			Instance.GetLinearYMotion(), Instance.GetLinearLimit(), EUERLJointCoordinate::LinearY, Joint);
		AppendLinearCoordinate(
			Instance.GetLinearZMotion(), Instance.GetLinearLimit(), EUERLJointCoordinate::LinearZ, Joint);
		AppendAngularCoordinate(
			Instance.GetAngularSwing1Motion(), Instance.GetAngularSwing1Limit(), EUERLJointCoordinate::Swing1, Joint);
		AppendAngularCoordinate(
			Instance.GetAngularSwing2Motion(), Instance.GetAngularSwing2Limit(), EUERLJointCoordinate::Swing2, Joint);
		AppendAngularCoordinate(
			Instance.GetAngularTwistMotion(), Instance.GetAngularTwistLimit(), EUERLJointCoordinate::Twist, Joint);
	}

	bool IsFinitePositionLimit(const FUERLJointTopology& Joint)
	{
		return !Joint.bHasPositionLimit
			|| (FMath::IsFinite(Joint.LowerLimit) && FMath::IsFinite(Joint.UpperLimit)
				&& Joint.LowerLimit <= Joint.UpperLimit);
	}

	void CanonicalizeTopology(FUERLRobotTopology& Topology)
	{
		const TArray<FName> OldBodyNames = Topology.BodyNames;
		const TArray<EUERLBodyMotionType> OldMotionTypes = Topology.BodyMotionTypes;
		const TArray<FUERLJointTopology> OldJoints = Topology.Joints;
		TArray<int32> BodyOrder;
		TArray<int32> JointOrder;
		TFunction<void(int32)> Visit = [&](int32 BodyIndex)
		{
			BodyOrder.Add(BodyIndex);
			TArray<int32> Children;
			for (int32 JointIndex = 0; JointIndex < OldJoints.Num(); ++JointIndex)
			{
				if (OldJoints[JointIndex].ParentBodyIndex == BodyIndex)
				{
					Children.Add(JointIndex);
				}
			}
			Children.Sort([&](int32 Left, int32 Right)
			{
				return OldBodyNames[OldJoints[Left].ChildBodyIndex].ToString()
					< OldBodyNames[OldJoints[Right].ChildBodyIndex].ToString();
			});
			for (int32 JointIndex : Children)
			{
				JointOrder.Add(JointIndex);
				Visit(OldJoints[JointIndex].ChildBodyIndex);
			}
		};
		Visit(Topology.RootBodyIndex);

		TArray<int32> NewBodyIndex;
		NewBodyIndex.Init(INDEX_NONE, OldBodyNames.Num());
		Topology.BodyNames.Reset(BodyOrder.Num());
		Topology.BodyMotionTypes.Reset(BodyOrder.Num());
		for (int32 OldBodyIndex : BodyOrder)
		{
			NewBodyIndex[OldBodyIndex] = Topology.BodyNames.Num();
			Topology.BodyNames.Add(OldBodyNames[OldBodyIndex]);
			Topology.BodyMotionTypes.Add(OldMotionTypes[OldBodyIndex]);
		}
		Topology.Joints.Reset(JointOrder.Num());
		for (int32 OldJointIndex : JointOrder)
		{
			FUERLJointTopology Joint = OldJoints[OldJointIndex];
			Joint.ParentBodyIndex = NewBodyIndex[Joint.ParentBodyIndex];
			Joint.ChildBodyIndex = NewBodyIndex[Joint.ChildBodyIndex];
			Topology.Joints.Add(MoveTemp(Joint));
		}
		Topology.RootBodyIndex = 0;
	}

	bool ReflectDefaultPositions(
		const USkeletalMesh& Mesh,
		FUERLRobotTopology& Topology,
		FString& OutError)
	{
		const FReferenceSkeleton& RefSkeleton = Mesh.GetRefSkeleton();
		for (FUERLJointTopology& Joint : Topology.Joints)
		{
			const int32 ChildBoneIndex = RefSkeleton.FindBoneIndex(Topology.BodyNames[Joint.ChildBodyIndex]);
			const int32 ParentBoneIndex = RefSkeleton.FindBoneIndex(Topology.BodyNames[Joint.ParentBodyIndex]);
			if (ChildBoneIndex == INDEX_NONE || ParentBoneIndex == INDEX_NONE)
			{
				OutError = FString::Printf(
					TEXT("robot SkeletalMesh reference pose is missing joint '%s' bodies"), *Joint.Name.ToString());
				return false;
			}
			const FTransform ChildBody = FAnimationRuntime::GetComponentSpaceTransformRefPose(
				RefSkeleton, ChildBoneIndex);
			const FTransform ParentBody = FAnimationRuntime::GetComponentSpaceTransformRefPose(
				RefSkeleton, ParentBoneIndex);
			const FTransform ChildFrame(
				Joint.ChildFrame.Rotation, Joint.ChildFrame.PositionMetres * 100.0, FVector::OneVector);
			const FTransform ParentFrame(
				Joint.ParentFrame.Rotation, Joint.ParentFrame.PositionMetres * 100.0, FVector::OneVector);
			Joint.DefaultPosition = MeasureGenericJointPosition(
				ComposeGenericConstraintFrameWorld(ChildFrame, ChildBody),
				ComposeGenericConstraintFrameWorld(ParentFrame, ParentBody),
				Joint.Coordinate);
			if (!FMath::IsFinite(Joint.DefaultPosition))
			{
				OutError = FString::Printf(
					TEXT("robot joint '%s' has a non-finite canonical position"), *Joint.Name.ToString());
				return false;
			}
		}
		return true;
	}
}

bool ReflectPhysicsAssetTopology(
	const UPhysicsAsset* PhysicsAsset,
	FUERLRobotTopology& OutTopology,
	FString& OutError)
{
	OutTopology = FUERLRobotTopology();
	OutError.Reset();

	if (!PhysicsAsset)
	{
		OutError = TEXT("robot asset has no PhysicsAsset to reflect");
		return false;
	}

	const TArray<TObjectPtr<USkeletalBodySetup>>& BodySetups = PhysicsAsset->SkeletalBodySetups;
	if (BodySetups.Num() == 0)
	{
		OutError = TEXT("robot PhysicsAsset declares no bodies");
		return false;
	}

	TMap<FName, int32> BodyIndexByBone;
	OutTopology.BodyNames.Reserve(BodySetups.Num());
	OutTopology.BodyMotionTypes.Reserve(BodySetups.Num());
	for (const TObjectPtr<USkeletalBodySetup>& BodySetup : BodySetups)
	{
		if (!BodySetup || BodySetup->BoneName.IsNone())
		{
			OutError = TEXT("robot PhysicsAsset contains a body with no bone name");
			return false;
		}
		if (BodyIndexByBone.Contains(BodySetup->BoneName))
		{
			OutError = FString::Printf(
				TEXT("robot PhysicsAsset has duplicate body bone '%s'"), *BodySetup->BoneName.ToString());
			return false;
		}
		if (BodySetup->BoneName.ToString().Contains(TEXT(".")))
		{
			OutError = FString::Printf(
				TEXT("robot PhysicsAsset body bone '%s' must not contain '.'"), *BodySetup->BoneName.ToString());
			return false;
		}

		BodyIndexByBone.Add(BodySetup->BoneName, OutTopology.BodyNames.Num());
		OutTopology.BodyNames.Add(BodySetup->BoneName);
		OutTopology.BodyMotionTypes.Add(
			BodySetup->PhysicsType == PhysType_Kinematic
				? EUERLBodyMotionType::Kinematic
				: EUERLBodyMotionType::Simulated);
	}

	const TArray<TObjectPtr<UPhysicsConstraintTemplate>>& Constraints = PhysicsAsset->ConstraintSetup;
	if (Constraints.Num() != OutTopology.BodyNames.Num() - 1)
	{
		OutError = FString::Printf(
			TEXT("robot PhysicsAsset must have exactly one parent constraint per non-root body (bodies=%d, constraints=%d)"),
			OutTopology.BodyNames.Num(), Constraints.Num());
		return false;
	}

	TSet<FName> JointNames;
	TArray<int32> ParentJointByBody;
	ParentJointByBody.Init(INDEX_NONE, OutTopology.BodyNames.Num());
	OutTopology.Joints.Reserve(Constraints.Num());
	for (const TObjectPtr<UPhysicsConstraintTemplate>& Constraint : Constraints)
	{
		if (!Constraint)
		{
			OutError = TEXT("robot PhysicsAsset contains a null constraint");
			return false;
		}

		const FConstraintInstance& Instance = Constraint->DefaultInstance;
		FUERLJointTopology Joint;
		Joint.Name = Instance.JointName;
		if (Joint.Name.IsNone())
		{
			OutError = TEXT("robot PhysicsAsset contains a constraint with no joint name");
			return false;
		}
		if (JointNames.Contains(Joint.Name))
		{
			OutError = FString::Printf(
				TEXT("robot PhysicsAsset has duplicate joint name '%s'"), *Joint.Name.ToString());
			return false;
		}
		if (Joint.Name.ToString().Contains(TEXT(".")))
		{
			OutError = FString::Printf(
				TEXT("robot PhysicsAsset joint name '%s' must not contain '.'"), *Joint.Name.ToString());
			return false;
		}
		JointNames.Add(Joint.Name);

		const int32* ChildIndex = BodyIndexByBone.Find(Instance.ConstraintBone1);
		const int32* ParentIndex = BodyIndexByBone.Find(Instance.ConstraintBone2);
		if (!ChildIndex || !ParentIndex)
		{
			OutError = FString::Printf(
				TEXT("constraint '%s' references unknown body (child '%s', parent '%s')"),
				*Joint.Name.ToString(),
				*Instance.ConstraintBone1.ToString(),
				*Instance.ConstraintBone2.ToString());
			return false;
		}
		if (*ChildIndex == *ParentIndex)
		{
			OutError = FString::Printf(
				TEXT("constraint '%s' cannot connect a body to itself"), *Joint.Name.ToString());
			return false;
		}
		if (ParentJointByBody[*ChildIndex] != INDEX_NONE)
		{
			OutError = FString::Printf(
				TEXT("body '%s' has more than one parent constraint"), *Instance.ConstraintBone1.ToString());
			return false;
		}

		Joint.ChildBodyIndex = *ChildIndex;
		Joint.ParentBodyIndex = *ParentIndex;
		ReflectConstraintCoordinates(Instance, Joint);
		if (Joint.DegreesOfFreedom != 1 || Joint.Coordinate == EUERLJointCoordinate::None)
		{
			OutError = FString::Printf(
				TEXT("constraint '%s' has %d unlocked coordinates, expected exactly one"),
				*Joint.Name.ToString(), Joint.DegreesOfFreedom);
			return false;
		}
		if (!IsFinitePositionLimit(Joint))
		{
			OutError = FString::Printf(
				TEXT("constraint '%s' has invalid position limits"), *Joint.Name.ToString());
			return false;
		}

		const FTransform ChildFrame = Instance.GetRefFrame(EConstraintFrame::Frame1);
		const FTransform ParentFrame = Instance.GetRefFrame(EConstraintFrame::Frame2);
		if (ChildFrame.ContainsNaN() || ParentFrame.ContainsNaN()
			|| !ChildFrame.GetRotation().IsNormalized() || !ParentFrame.GetRotation().IsNormalized())
		{
			OutError = FString::Printf(
				TEXT("constraint '%s' has an invalid constraint frame"), *Joint.Name.ToString());
			return false;
		}
		Joint.ChildFrame.bValid = true;
		Joint.ChildFrame.PositionMetres = ChildFrame.GetLocation() / 100.0f;
		Joint.ChildFrame.Rotation = ChildFrame.GetRotation();
		Joint.ParentFrame.bValid = true;
		Joint.ParentFrame.PositionMetres = ParentFrame.GetLocation() / 100.0f;
		Joint.ParentFrame.Rotation = ParentFrame.GetRotation();

		ParentJointByBody[*ChildIndex] = OutTopology.Joints.Num();
		OutTopology.Joints.Add(MoveTemp(Joint));
	}

	TArray<int32> RootCandidates;
	for (int32 BodyIndex = 0; BodyIndex < ParentJointByBody.Num(); ++BodyIndex)
	{
		if (ParentJointByBody[BodyIndex] == INDEX_NONE)
		{
			RootCandidates.Add(BodyIndex);
		}
	}
	if (RootCandidates.Num() != 1)
	{
		OutError = FString::Printf(
			TEXT("robot PhysicsAsset must have exactly one topology root, found %d"), RootCandidates.Num());
		return false;
	}
	OutTopology.RootBodyIndex = RootCandidates[0];

	TArray<bool> Reachable;
	Reachable.Init(false, OutTopology.BodyNames.Num());
	TArray<int32> Pending;
	Pending.Add(OutTopology.RootBodyIndex);
	while (Pending.Num() > 0)
	{
		const int32 BodyIndex = Pending.Pop(EAllowShrinking::No);
		if (Reachable[BodyIndex])
		{
			continue;
		}
		Reachable[BodyIndex] = true;
		for (const FUERLJointTopology& Joint : OutTopology.Joints)
		{
			if (Joint.ParentBodyIndex == BodyIndex && !Reachable[Joint.ChildBodyIndex])
			{
				Pending.Add(Joint.ChildBodyIndex);
			}
		}
	}
	for (int32 BodyIndex = 0; BodyIndex < Reachable.Num(); ++BodyIndex)
	{
		if (!Reachable[BodyIndex])
		{
			OutError = FString::Printf(
				TEXT("robot PhysicsAsset body '%s' is disconnected from the topology root"),
				*OutTopology.BodyNames[BodyIndex].ToString());
			return false;
		}
	}

	int32 KinematicBodyCount = 0;
	for (int32 BodyIndex = 0; BodyIndex < OutTopology.BodyMotionTypes.Num(); ++BodyIndex)
	{
		if (OutTopology.BodyMotionTypes[BodyIndex] == EUERLBodyMotionType::Kinematic)
		{
			++KinematicBodyCount;
		}
	}
	if (KinematicBodyCount > 1
		|| (KinematicBodyCount == 1
			&& OutTopology.BodyMotionTypes[OutTopology.RootBodyIndex] != EUERLBodyMotionType::Kinematic))
	{
		OutError = TEXT("robot PhysicsAsset allows only one kinematic body and it must be the topology root");
		return false;
	}
	OutTopology.bFixedBase =
		OutTopology.BodyMotionTypes[OutTopology.RootBodyIndex] == EUERLBodyMotionType::Kinematic;
	CanonicalizeTopology(OutTopology);

	UE_LOG(LogUERLRobot, Log,
		TEXT("reflected robot topology: %d bodies, %d joints, root=%d, fixed_base=%s"),
		OutTopology.BodyNames.Num(), OutTopology.Joints.Num(), OutTopology.RootBodyIndex,
		OutTopology.bFixedBase ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUERLTopologyReflector::ReflectSkeletalMesh(
	const USkeletalMesh* SkeletalMesh,
	FUERLRobotTopology& OutTopology,
	FString& OutError)
{
	if (!SkeletalMesh)
	{
		OutError = TEXT("robot asset is not a SkeletalMesh");
		return false;
	}
	if (!ReflectPhysicsAssetTopology(SkeletalMesh->GetPhysicsAsset(), OutTopology, OutError))
	{
		return false;
	}
	return ReflectDefaultPositions(*SkeletalMesh, OutTopology, OutError);
}

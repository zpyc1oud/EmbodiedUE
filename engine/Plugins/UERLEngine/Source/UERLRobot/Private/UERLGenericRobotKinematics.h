#pragma once

#include "CoreMinimal.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "UERLInterfaceTypes.h"

/** Resolve the unique unlocked coordinate of a scalar PhysicsAsset constraint. */
UERLROBOT_API bool ResolveGenericJointCoordinate(
	const FConstraintInstance& Instance,
	EUERLJointCoordinate& OutCoordinate,
	FString& OutError,
	int32 JointIndex);

/** Return the coordinate-space axis used by the UE Chaos constraint API. */
UERLROBOT_API FVector GenericJointCoordinateAxis(EUERLJointCoordinate Coordinate);

/** Return whether a coordinate is angular rather than linear. */
UERLROBOT_API bool IsGenericAngularCoordinate(EUERLJointCoordinate Coordinate);

/** Convert SI generalized effort to the centimeter-based units consumed by Chaos. */
UERLROBOT_API double ConvertGenericJointEffortToChaos(
	double EffortSi,
	EUERLJointCoordinate Coordinate);

/** Compose a body-local constraint reference frame into world space. */
UERLROBOT_API FTransform ComposeGenericConstraintFrameWorld(
	const FTransform& RefFrameLocal,
	const FTransform& BodyWorld);

/** Recover a body world transform from its body-local constraint frame. */
UERLROBOT_API FTransform RecoverGenericBodyWorldFromConstraintFrame(
	const FTransform& RefFrameLocal,
	const FTransform& FrameWorld);

/** Compose the asset's root-body reference offset with a requested Robot root pose. */
UERLROBOT_API FTransform ComposeGenericRootBodyWorld(
	const FTransform& RootBodyReference,
	const FTransform& RobotRootWorld);

/** Recover the Robot root pose represented by a root physics body. */
UERLROBOT_API FTransform RecoverGenericRobotRootWorld(
	const FTransform& RootBodyReference,
	const FTransform& RootBodyWorld);

/** Measure one scalar joint position from its child and parent constraint frames. */
UERLROBOT_API double MeasureGenericJointPosition(
	const FTransform& ChildFrameWorld,
	const FTransform& ParentFrameWorld,
	EUERLJointCoordinate Coordinate);

/** World-space rigid-body state used while rebasing a floating Robot root. */
struct FUERLGenericBodyState
{
	FTransform Transform = FTransform::Identity;
	FVector LinearVelocity = FVector::ZeroVector;
	FVector AngularVelocity = FVector::ZeroVector;
};

/** Rebase one body with its root while preserving root-relative pose and motion. */
UERLROBOT_API FUERLGenericBodyState RebaseGenericBodyState(
	const FUERLGenericBodyState& Body,
	const FUERLGenericBodyState& CurrentRoot,
	const FUERLGenericBodyState& TargetRoot);

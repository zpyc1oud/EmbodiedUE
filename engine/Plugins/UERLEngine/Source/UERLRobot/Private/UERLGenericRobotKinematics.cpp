#include "UERLGenericRobotKinematics.h"

bool ResolveGenericJointCoordinate(
	const FConstraintInstance& Instance,
	EUERLJointCoordinate& OutCoordinate,
	FString& OutError,
	int32 JointIndex)
{
	TArray<EUERLJointCoordinate> Coordinates;
	if (Instance.GetLinearXMotion() != ELinearConstraintMotion::LCM_Locked)
	{
		Coordinates.Add(EUERLJointCoordinate::LinearX);
	}
	if (Instance.GetLinearYMotion() != ELinearConstraintMotion::LCM_Locked)
	{
		Coordinates.Add(EUERLJointCoordinate::LinearY);
	}
	if (Instance.GetLinearZMotion() != ELinearConstraintMotion::LCM_Locked)
	{
		Coordinates.Add(EUERLJointCoordinate::LinearZ);
	}
	if (Instance.GetAngularSwing1Motion() != EAngularConstraintMotion::ACM_Locked)
	{
		Coordinates.Add(EUERLJointCoordinate::Swing1);
	}
	if (Instance.GetAngularSwing2Motion() != EAngularConstraintMotion::ACM_Locked)
	{
		Coordinates.Add(EUERLJointCoordinate::Swing2);
	}
	if (Instance.GetAngularTwistMotion() != EAngularConstraintMotion::ACM_Locked)
	{
		Coordinates.Add(EUERLJointCoordinate::Twist);
	}
	if (Coordinates.Num() != 1)
	{
		OutCoordinate = EUERLJointCoordinate::None;
		OutError = FString::Printf(
			TEXT("generic Robot joint %d has %d unlocked coordinates, expected one"),
			JointIndex,
			Coordinates.Num());
		return false;
	}
	OutCoordinate = Coordinates[0];
	return true;
}

FVector GenericJointCoordinateAxis(EUERLJointCoordinate Coordinate)
{
	// UE Chaos defines twist=X, swing1=Z, and swing2=Y; this matches
	// FChaosEngineInterface::GetCurrentSwing1/2/Twist.
	switch (Coordinate)
	{
	case EUERLJointCoordinate::LinearX:
	case EUERLJointCoordinate::Twist:
		return FVector::XAxisVector;
	case EUERLJointCoordinate::LinearY:
	case EUERLJointCoordinate::Swing2:
		return FVector::YAxisVector;
	case EUERLJointCoordinate::LinearZ:
	case EUERLJointCoordinate::Swing1:
		return FVector::ZAxisVector;
	default:
		checkNoEntry();
		return FVector::ZeroVector;
	}
}

bool IsGenericAngularCoordinate(EUERLJointCoordinate Coordinate)
{
	return Coordinate == EUERLJointCoordinate::Swing1
		|| Coordinate == EUERLJointCoordinate::Swing2
		|| Coordinate == EUERLJointCoordinate::Twist;
}

double ConvertGenericJointEffortToChaos(
	double EffortSi,
	EUERLJointCoordinate Coordinate)
{
	return EffortSi * (IsGenericAngularCoordinate(Coordinate) ? 10000.0 : 100.0);
}

FTransform ComposeGenericConstraintFrameWorld(
	const FTransform& RefFrameLocal,
	const FTransform& BodyWorld)
{
	return RefFrameLocal * BodyWorld;
}

FTransform RecoverGenericBodyWorldFromConstraintFrame(
	const FTransform& RefFrameLocal,
	const FTransform& FrameWorld)
{
	return RefFrameLocal.Inverse() * FrameWorld;
}

FTransform ComposeGenericRootBodyWorld(
	const FTransform& RootBodyReference,
	const FTransform& RobotRootWorld)
{
	return RootBodyReference * RobotRootWorld;
}

FTransform RecoverGenericRobotRootWorld(
	const FTransform& RootBodyReference,
	const FTransform& RootBodyWorld)
{
	return RootBodyReference.Inverse() * RootBodyWorld;
}

double MeasureGenericJointPosition(
	const FTransform& ChildFrameWorld,
	const FTransform& ParentFrameWorld,
	EUERLJointCoordinate Coordinate)
{
	const FVector AxisWorld = ChildFrameWorld.TransformVectorNoScale(
		GenericJointCoordinateAxis(Coordinate)).GetSafeNormal();
	checkf(!AxisWorld.IsNearlyZero(), TEXT("generic joint constraint axis is zero"));
	if (IsGenericAngularCoordinate(Coordinate))
	{
		const FQuat RelativeRotation = ChildFrameWorld.GetRelativeTransform(
			ParentFrameWorld).GetRotation().GetNormalized();
		return RelativeRotation.GetTwistAngle(GenericJointCoordinateAxis(Coordinate));
	}
	return FVector::DotProduct(
		ChildFrameWorld.GetLocation() - ParentFrameWorld.GetLocation(), AxisWorld) / 100.0;
}

FUERLGenericBodyState RebaseGenericBodyState(
	const FUERLGenericBodyState& Body,
	const FUERLGenericBodyState& CurrentRoot,
	const FUERLGenericBodyState& TargetRoot)
{
	const FQuat CurrentRootRotation = CurrentRoot.Transform.GetRotation().GetNormalized();
	const FQuat TargetRootRotation = TargetRoot.Transform.GetRotation().GetNormalized();
	const FVector CurrentOffset = Body.Transform.GetLocation() - CurrentRoot.Transform.GetLocation();
	const FVector RelativeLinearVelocity = CurrentRootRotation.UnrotateVector(
		Body.LinearVelocity - CurrentRoot.LinearVelocity
		- FVector::CrossProduct(CurrentRoot.AngularVelocity, CurrentOffset));
	const FVector RelativeAngularVelocity = CurrentRootRotation.UnrotateVector(
		Body.AngularVelocity - CurrentRoot.AngularVelocity);

	FUERLGenericBodyState Result;
	Result.Transform = Body.Transform.GetRelativeTransform(CurrentRoot.Transform) * TargetRoot.Transform;
	const FVector TargetOffset = Result.Transform.GetLocation() - TargetRoot.Transform.GetLocation();
	Result.LinearVelocity = TargetRoot.LinearVelocity
		+ FVector::CrossProduct(TargetRoot.AngularVelocity, TargetOffset)
		+ TargetRootRotation.RotateVector(RelativeLinearVelocity);
	Result.AngularVelocity = TargetRoot.AngularVelocity
		+ TargetRootRotation.RotateVector(RelativeAngularVelocity);
	return Result;
}

#include "Misc/AutomationTest.h"

#include "UERLGenericRobotKinematics.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FConstraintInstance ScalarConstraint()
	{
		FConstraintInstance Instance;
		Instance.SetLinearXMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetLinearYMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetLinearZMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Locked);
		Instance.SetAngularSwing2Motion(EAngularConstraintMotion::ACM_Locked);
		Instance.SetAngularTwistMotion(EAngularConstraintMotion::ACM_Locked);
		return Instance;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsAxisTest,
	"UERL.Unit.Robot.GenericKinematics.CoordinateAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsAxisTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Chaos twist uses X"), GenericJointCoordinateAxis(EUERLJointCoordinate::Twist), FVector::XAxisVector);
	TestEqual(TEXT("Chaos swing1 uses Z"), GenericJointCoordinateAxis(EUERLJointCoordinate::Swing1), FVector::ZAxisVector);
	TestEqual(TEXT("Chaos swing2 uses Y"), GenericJointCoordinateAxis(EUERLJointCoordinate::Swing2), FVector::YAxisVector);
	TestEqual(TEXT("linear X uses X"), GenericJointCoordinateAxis(EUERLJointCoordinate::LinearX), FVector::XAxisVector);
	TestFalse(TEXT("linear X is not angular"), IsGenericAngularCoordinate(EUERLJointCoordinate::LinearX));
	TestTrue(TEXT("twist is angular"), IsGenericAngularCoordinate(EUERLJointCoordinate::Twist));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-001: Chaos coordinate axes match constraint API"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsResolveTest,
	"UERL.Unit.Robot.GenericKinematics.ResolveScalarCoordinate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsResolveTest::RunTest(const FString& Parameters)
{
	FString Error;
	EUERLJointCoordinate Coordinate = EUERLJointCoordinate::None;

	FConstraintInstance Twist = ScalarConstraint();
	Twist.SetAngularTwistMotion(EAngularConstraintMotion::ACM_Free);
	TestTrue(TEXT("free twist resolves"), ResolveGenericJointCoordinate(Twist, Coordinate, Error, 0));
	TestEqual(TEXT("free twist coordinate"), Coordinate, EUERLJointCoordinate::Twist);

	FConstraintInstance Swing1 = ScalarConstraint();
	Swing1.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Free);
	Error.Reset();
	TestTrue(TEXT("free swing1 resolves"), ResolveGenericJointCoordinate(Swing1, Coordinate, Error, 1));
	TestEqual(TEXT("free swing1 coordinate"), Coordinate, EUERLJointCoordinate::Swing1);

	FConstraintInstance Linear = ScalarConstraint();
	Linear.SetLinearYMotion(ELinearConstraintMotion::LCM_Free);
	Error.Reset();
	TestTrue(TEXT("free linear Y resolves"), ResolveGenericJointCoordinate(Linear, Coordinate, Error, 2));
	TestEqual(TEXT("free linear Y coordinate"), Coordinate, EUERLJointCoordinate::LinearY);

	FConstraintInstance MultiAxis = ScalarConstraint();
	MultiAxis.SetAngularTwistMotion(EAngularConstraintMotion::ACM_Free);
	MultiAxis.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Free);
	Error.Reset();
	TestFalse(TEXT("multi-axis joint is rejected for scalar binding"),
		ResolveGenericJointCoordinate(MultiAxis, Coordinate, Error, 3));
	TestFalse(TEXT("multi-axis rejection has a diagnostic"), Error.IsEmpty());
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-002: scalar joint coordinate resolution rejects ambiguity"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsEffortUnitsTest,
	"UERL.Unit.Robot.GenericKinematics.AC_UE_UNIT_ROBOT_KIN_003.EffortUnits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsEffortUnitsTest::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("one newton converts to one hundred Chaos force units"),
		ConvertGenericJointEffortToChaos(1.0, EUERLJointCoordinate::LinearX),
		100.0);
	TestEqual(
		TEXT("one newton metre converts to ten thousand Chaos torque units"),
		ConvertGenericJointEffortToChaos(1.0, EUERLJointCoordinate::Twist),
		10000.0);
	TestEqual(
		TEXT("torque conversion preserves sign"),
		ConvertGenericJointEffortToChaos(-0.5, EUERLJointCoordinate::Swing1),
		-5000.0);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-003: generalized effort uses dimensionally correct Chaos units"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsConstraintFrameTest,
	"UERL.Unit.Robot.GenericKinematics.AC_UE_UNIT_ROBOT_KIN_004.ConstraintFrameWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsConstraintFrameTest::RunTest(const FString& Parameters)
{
	const FTransform RefFrameLocal(FQuat::Identity, FVector(100.0, 0.0, 0.0));
	const FTransform BodyWorld(
		FQuat(FVector::ZAxisVector, HALF_PI),
		FVector(10.0, 20.0, 30.0));

	const FTransform FrameWorld = ComposeGenericConstraintFrameWorld(RefFrameLocal, BodyWorld);
	TestTrue(
		TEXT("body rotation carries the local anchor into world Y"),
		FrameWorld.GetLocation().Equals(FVector(10.0, 120.0, 30.0), KINDA_SMALL_NUMBER));
	TestTrue(
		TEXT("identity local rotation preserves the body world rotation"),
		FrameWorld.GetRotation().Equals(BodyWorld.GetRotation(), KINDA_SMALL_NUMBER));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-004: body-local constraint frames compose into world space"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsRecoverBodyTest,
	"UERL.Unit.Robot.GenericKinematics.AC_UE_UNIT_ROBOT_KIN_005.RecoverBodyWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsRecoverBodyTest::RunTest(const FString& Parameters)
{
	const FTransform RefFrameLocal(FQuat::Identity, FVector(100.0, 0.0, 0.0));
	const FQuat QuarterTurn(FVector::ZAxisVector, HALF_PI);
	const FTransform FrameWorld(QuarterTurn, FVector(10.0, 120.0, 30.0));

	const FTransform BodyWorld = RecoverGenericBodyWorldFromConstraintFrame(RefFrameLocal, FrameWorld);
	TestTrue(
		TEXT("recovering the body removes the rotated local anchor"),
		BodyWorld.GetLocation().Equals(FVector(10.0, 20.0, 30.0), KINDA_SMALL_NUMBER));
	TestTrue(
		TEXT("identity local rotation preserves the recovered body rotation"),
		BodyWorld.GetRotation().Equals(QuarterTurn, KINDA_SMALL_NUMBER));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-005: reset recovers body world pose from the desired constraint frame"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsRootTreeRebaseTest,
	"UERL.Unit.Robot.GenericKinematics.AC_UE_UNIT_ROBOT_KIN_006.RootTreeRebase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsRootTreeRebaseTest::RunTest(const FString& Parameters)
{
	FUERLGenericBodyState CurrentRoot;
	FUERLGenericBodyState Child;
	Child.Transform.SetLocation(FVector(100.0, 0.0, 0.0));
	Child.LinearVelocity = FVector(0.0, 100.0, 0.0);
	Child.AngularVelocity = FVector(0.0, 0.0, 1.0);

	FUERLGenericBodyState TargetRoot;
	TargetRoot.Transform = FTransform(
		FQuat(FVector::ZAxisVector, HALF_PI),
		FVector(10.0, 20.0, 0.0));
	TargetRoot.LinearVelocity = FVector(5.0, 6.0, 0.0);
	TargetRoot.AngularVelocity = FVector(0.0, 0.0, 2.0);

	const FUERLGenericBodyState RebasedRoot = RebaseGenericBodyState(CurrentRoot, CurrentRoot, TargetRoot);
	const FUERLGenericBodyState RebasedChild = RebaseGenericBodyState(Child, CurrentRoot, TargetRoot);
	TestTrue(TEXT("root reaches the requested transform"), RebasedRoot.Transform.Equals(TargetRoot.Transform));
	TestTrue(TEXT("root reaches the requested linear velocity"),
		RebasedRoot.LinearVelocity.Equals(TargetRoot.LinearVelocity));
	TestTrue(TEXT("root reaches the requested angular velocity"),
		RebasedRoot.AngularVelocity.Equals(TargetRoot.AngularVelocity));
	TestTrue(TEXT("child pose remains rigidly attached to the rebased root"),
		RebasedChild.Transform.Equals(FTransform(
			TargetRoot.Transform.GetRotation(), FVector(10.0, 120.0, 0.0))));
	TestTrue(TEXT("child preserves relative linear motion around the rebased root"),
		RebasedChild.LinearVelocity.Equals(FVector(-295.0, 6.0, 0.0)));
	TestTrue(TEXT("child preserves relative angular motion around the rebased root"),
		RebasedChild.AngularVelocity.Equals(FVector(0.0, 0.0, 3.0)));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-006: floating root reset rebases the complete body tree"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsDefaultPositionTest,
	"UERL.Unit.Robot.GenericKinematics.AC_UE_UNIT_ROBOT_KIN_007.DefaultPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsDefaultPositionTest::RunTest(const FString& Parameters)
{
	const FTransform ParentFrame(
		FQuat(FVector::ZAxisVector, 0.4),
		FVector(20.0, 30.0, 40.0));
	const FTransform RevoluteChild = FTransform(
		FQuat(FVector::XAxisVector, 0.35)) * ParentFrame;
	TestTrue(TEXT("revolute reference pose preserves its signed canonical angle"), FMath::IsNearlyEqual(
		MeasureGenericJointPosition(RevoluteChild, ParentFrame, EUERLJointCoordinate::Twist),
		0.35, 1.0e-6));

	FTransform PrismaticChild = ParentFrame;
	PrismaticChild.AddToTranslation(
		ParentFrame.TransformVectorNoScale(FVector::XAxisVector) * 25.0);
	TestTrue(TEXT("prismatic reference pose converts canonical displacement to metres"), FMath::IsNearlyEqual(
		MeasureGenericJointPosition(PrismaticChild, ParentFrame, EUERLJointCoordinate::LinearX),
		0.25, 1.0e-6));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-007: asset reference frames expose canonical joint position"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotKinematicsRootReferenceTest,
	"UERL.Unit.Robot.GenericKinematics.AC_UE_UNIT_ROBOT_KIN_008.RootReference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotKinematicsRootReferenceTest::RunTest(const FString& Parameters)
{
	const FTransform RootBodyReference(
		FQuat(FVector::YAxisVector, HALF_PI),
		FVector(0.0, 0.0, -14.0));
	const FTransform RequestedRobotRoot(
		FQuat(FVector::ZAxisVector, 0.4),
		FVector(100.0, 200.0, 18.0));
	const FTransform RootBodyWorld = ComposeGenericRootBodyWorld(
		RootBodyReference, RequestedRobotRoot);
	TestTrue(TEXT("asset root-body offset composes into the requested Robot pose"),
		RootBodyWorld.Equals(RootBodyReference * RequestedRobotRoot));
	TestTrue(TEXT("root-body observation recovers the requested Robot pose"),
		RecoverGenericRobotRootWorld(RootBodyReference, RootBodyWorld).Equals(RequestedRobotRoot));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-KIN-008: imported root-body offsets round-trip through reset and observation"));
	return true;
}

#endif

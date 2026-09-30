#include "Misc/AutomationTest.h"

#include "Engine/SkeletalMesh.h"
#include "UERLTopologyReflector.h"
#include "UERLTopologyReflectionInternal.h"

#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Add a named body to the PhysicsAsset and return its bone name. */
	FName AddBody(
		UPhysicsAsset& Asset,
		const TCHAR* BoneName,
		EPhysicsType PhysicsType = PhysType_Default)
	{
		USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(&Asset);
		Body->BoneName = FName(BoneName);
		Body->PhysicsType = PhysicsType;
		Asset.SkeletalBodySetups.Add(Body);
		return Body->BoneName;
	}

	/**
	 * Add a constraint wiring Child relative to Parent with the supplied angular
	 * twist motion, mirroring how a PhysicsAsset stores joint limits.
	 */
	void AddConstraint(
		UPhysicsAsset& Asset,
		const TCHAR* JointName,
		FName ParentBone,
		FName ChildBone,
		EAngularConstraintMotion TwistMotion,
		float TwistLimitDegrees,
		const FTransform& ParentFrame = FTransform::Identity,
		const FTransform& ChildFrame = FTransform::Identity)
	{
		UPhysicsConstraintTemplate* Constraint = NewObject<UPhysicsConstraintTemplate>(&Asset);
		FConstraintInstance& Instance = Constraint->DefaultInstance;
		Instance.JointName = FName(JointName);
		Instance.ConstraintBone2 = ParentBone;
		Instance.ConstraintBone1 = ChildBone;
		Instance.SetLinearXMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetLinearYMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetLinearZMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Locked);
		Instance.SetAngularSwing2Motion(EAngularConstraintMotion::ACM_Locked);
		Instance.SetAngularTwistMotion(TwistMotion);
		Instance.SetAngularTwistLimit(TwistMotion, TwistLimitDegrees);
		Instance.SetRefFrame(EConstraintFrame::Frame1, ChildFrame);
		Instance.SetRefFrame(EConstraintFrame::Frame2, ParentFrame);
		Asset.ConstraintSetup.Add(Constraint);
	}

	/** Add a constraint with one linear X coordinate and all other axes locked. */
	void AddPrismaticConstraint(
		UPhysicsAsset& Asset,
		const TCHAR* JointName,
		FName ParentBone,
		FName ChildBone,
		ELinearConstraintMotion XMotion,
		float LimitCentimetres,
		const FTransform& ParentFrame = FTransform::Identity,
		const FTransform& ChildFrame = FTransform::Identity)
	{
		UPhysicsConstraintTemplate* Constraint = NewObject<UPhysicsConstraintTemplate>(&Asset);
		FConstraintInstance& Instance = Constraint->DefaultInstance;
		Instance.JointName = FName(JointName);
		Instance.ConstraintBone2 = ParentBone;
		Instance.ConstraintBone1 = ChildBone;
		Instance.SetLinearXLimit(XMotion, LimitCentimetres);
		Instance.SetLinearYMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetLinearZMotion(ELinearConstraintMotion::LCM_Locked);
		Instance.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Locked);
		Instance.SetAngularSwing2Motion(EAngularConstraintMotion::ACM_Locked);
		Instance.SetAngularTwistMotion(EAngularConstraintMotion::ACM_Locked);
		Instance.SetRefFrame(EConstraintFrame::Frame1, ChildFrame);
		Instance.SetRefFrame(EConstraintFrame::Frame2, ParentFrame);
		Asset.ConstraintSetup.Add(Constraint);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTopologyReflectorCartPoleTest,
	"UERL.Unit.Robot.TopologyReflector.CartPole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTopologyReflectorCartPoleTest::RunTest(const FString& Parameters)
{
	UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(GetTransientPackageAsObject());
	const FName Cart = AddBody(*Asset, TEXT("cart"), PhysType_Kinematic);
	const FName Pole = AddBody(*Asset, TEXT("pole"));
	// The pole is a free-swinging revolute joint; UE stores the child as Bone1.
	AddConstraint(*Asset, TEXT("cart_to_pole"), Cart, Pole,
		EAngularConstraintMotion::ACM_Free, 0.0f);

	FUERLRobotTopology Topology;
	FString Error;
	const bool bReflected = ReflectPhysicsAssetTopology(Asset, Topology, Error);

	TestTrue(TEXT("cartpole topology reflects"), bReflected);
	TestEqual(TEXT("two bodies"), Topology.BodyNames.Num(), 2);
	TestEqual(TEXT("body 0 is cart"), Topology.BodyNames[0], Cart);
	TestEqual(TEXT("body 1 is pole"), Topology.BodyNames[1], Pole);
	TestEqual(TEXT("one joint"), Topology.Joints.Num(), 1);
	TestEqual(TEXT("one root"), Topology.RootBodyIndex, 0);
	TestTrue(TEXT("kinematic cart marks fixed base"), Topology.bFixedBase);
	if (Topology.Joints.Num() == 1)
	{
		const FUERLJointTopology& Joint = Topology.Joints[0];
		TestEqual(TEXT("joint parent is cart index"), Joint.ParentBodyIndex, 0);
		TestEqual(TEXT("joint child is pole index"), Joint.ChildBodyIndex, 1);
		TestEqual(TEXT("one free angular dof"), Joint.DegreesOfFreedom, 1);
		TestEqual(TEXT("joint coordinate is twist"), Joint.Coordinate, EUERLJointCoordinate::Twist);
		TestEqual(TEXT("joint coordinate type is revolute"), Joint.CoordinateType, EUERLJointCoordinateType::Revolute);
		TestFalse(TEXT("free joint has no finite position limit"), Joint.bHasPositionLimit);
	}
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-TOPO-001: reflects cartpole bodies and free pole joint"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTopologyReflectorFailLoudlyTest,
	"UERL.Unit.Robot.TopologyReflector.FailLoudly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTopologyReflectorFailLoudlyTest::RunTest(const FString& Parameters)
{
	FUERLRobotTopology Topology;
	FString Error;

	// A null PhysicsAsset is a hard structural error, never a silent downgrade.
	TestFalse(TEXT("null asset fails"), ReflectPhysicsAssetTopology(nullptr, Topology, Error));
	TestFalse(TEXT("null asset reports error"), Error.IsEmpty());

	// A constraint referencing an unknown body is a hard structural error.
	UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(GetTransientPackageAsObject());
	AddBody(*Asset, TEXT("cart"));
	AddConstraint(*Asset, TEXT("cart_to_ghost"), FName(TEXT("cart")), FName(TEXT("ghost")),
		EAngularConstraintMotion::ACM_Free, 0.0f);
	Error.Reset();
	TestFalse(TEXT("unknown body fails"), ReflectPhysicsAssetTopology(Asset, Topology, Error));
	TestFalse(TEXT("unknown body reports error"), Error.IsEmpty());

	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-TOPO-002: structurally invalid assets fail loudly"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTopologyReflectorMetadataTest,
	"UERL.Unit.Robot.TopologyReflector.Metadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTopologyReflectorMetadataTest::RunTest(const FString& Parameters)
{
	UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(GetTransientPackageAsObject());
	const FName Base = AddBody(*Asset, TEXT("base"), PhysType_Kinematic);
	const FName Cart = AddBody(*Asset, TEXT("cart"));
	const FName Pole = AddBody(*Asset, TEXT("pole"));
	const FTransform ParentFrame(FQuat::MakeFromEuler(FVector(0.0f, 0.0f, 90.0f)), FVector(10.0f, 20.0f, 30.0f));
	const FTransform ChildFrame(FQuat::MakeFromEuler(FVector(0.0f, 45.0f, 0.0f)), FVector(-5.0f, 0.0f, 15.0f));
	AddPrismaticConstraint(*Asset, TEXT("cart"), Base, Cart, ELinearConstraintMotion::LCM_Limited, 50.0f, ParentFrame, ChildFrame);
	AddConstraint(*Asset, TEXT("pole"), Cart, Pole, EAngularConstraintMotion::ACM_Free, 0.0f, ParentFrame, ChildFrame);

	FUERLRobotTopology Topology;
	FString Error;
	TestTrue(TEXT("fixed-base chain reflects"), ReflectPhysicsAssetTopology(Asset, Topology, Error));
	if (!Topology.BodyNames.IsValidIndex(2) || Topology.Joints.Num() != 2)
	{
		return false;
	}
	TestEqual(TEXT("root is base"), Topology.RootBodyIndex, 0);
	TestTrue(TEXT("kinematic root marks fixed base"), Topology.bFixedBase);
	TestEqual(TEXT("base is kinematic"), Topology.BodyMotionTypes[0], EUERLBodyMotionType::Kinematic);
	TestEqual(TEXT("cart is simulated"), Topology.BodyMotionTypes[1], EUERLBodyMotionType::Simulated);
	TestEqual(TEXT("pole is simulated"), Topology.BodyMotionTypes[2], EUERLBodyMotionType::Simulated);
	TestEqual(TEXT("cart coordinate is prismatic"), Topology.Joints[0].Coordinate, EUERLJointCoordinate::LinearX);
	TestEqual(TEXT("cart coordinate type is prismatic"), Topology.Joints[0].CoordinateType, EUERLJointCoordinateType::Prismatic);
	TestEqual(TEXT("cart has a position limit"), Topology.Joints[0].bHasPositionLimit, true);
	TestTrue(TEXT("cart lower limit is converted to meters"), FMath::IsNearlyEqual(Topology.Joints[0].LowerLimit, -0.5));
	TestTrue(TEXT("cart upper limit is converted to meters"), FMath::IsNearlyEqual(Topology.Joints[0].UpperLimit, 0.5));
	TestTrue(TEXT("child frame is valid"), Topology.Joints[0].ChildFrame.bValid);
	TestTrue(TEXT("parent frame is valid"), Topology.Joints[0].ParentFrame.bValid);
	TestTrue(TEXT("child frame position is converted to meters"), FMath::IsNearlyEqual(Topology.Joints[0].ChildFrame.PositionMetres.X, -0.05));
	TestTrue(TEXT("parent frame position is converted to meters"), FMath::IsNearlyEqual(Topology.Joints[0].ParentFrame.PositionMetres.X, 0.1));
	TestTrue(TEXT("child frame rotation is preserved"), Topology.Joints[0].ChildFrame.Rotation.Equals(ChildFrame.GetRotation().GetNormalized()));
	TestTrue(TEXT("parent frame rotation is preserved"), Topology.Joints[0].ParentFrame.Rotation.Equals(ParentFrame.GetRotation().GetNormalized()));
	TestTrue(TEXT("child and parent frames remain distinct"), !Topology.Joints[0].ChildFrame.Rotation.Equals(Topology.Joints[0].ParentFrame.Rotation));
	TestEqual(TEXT("pole coordinate is twist"), Topology.Joints[1].Coordinate, EUERLJointCoordinate::Twist);
	TestEqual(TEXT("pole coordinate type is revolute"), Topology.Joints[1].CoordinateType, EUERLJointCoordinateType::Revolute);
	TestFalse(TEXT("free pole has no position limit"), Topology.Joints[1].bHasPositionLimit);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-TOPO-003: fixed-base metadata, coordinate type, and SI limits"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTopologyReflectorStructureTest,
	"UERL.Unit.Robot.TopologyReflector.Structure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTopologyReflectorStructureTest::RunTest(const FString& Parameters)
{
	FString Error;
	FUERLRobotTopology Topology;

	UPhysicsAsset* MultiAxisAsset = NewObject<UPhysicsAsset>(GetTransientPackageAsObject());
	const FName Base = AddBody(*MultiAxisAsset, TEXT("base"), PhysType_Kinematic);
	const FName Link = AddBody(*MultiAxisAsset, TEXT("link"));
	AddConstraint(*MultiAxisAsset, TEXT("multi"), Base, Link, EAngularConstraintMotion::ACM_Free, 0.0f);
	UPhysicsConstraintTemplate* ExtraAxis = NewObject<UPhysicsConstraintTemplate>(MultiAxisAsset);
	ExtraAxis->DefaultInstance.JointName = FName(TEXT("multi"));
	ExtraAxis->DefaultInstance.ConstraintBone2 = Base;
	ExtraAxis->DefaultInstance.ConstraintBone1 = Link;
	ExtraAxis->DefaultInstance.SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Free);
	MultiAxisAsset->ConstraintSetup.Add(ExtraAxis);
	TestFalse(TEXT("multiple constraints for one child fail"), ReflectPhysicsAssetTopology(MultiAxisAsset, Topology, Error));
	TestFalse(TEXT("multiple constraints report a diagnostic"), Error.IsEmpty());

	UPhysicsAsset* DisconnectedAsset = NewObject<UPhysicsAsset>(GetTransientPackageAsObject());
	const FName Root = AddBody(*DisconnectedAsset, TEXT("root"), PhysType_Kinematic);
	const FName Child = AddBody(*DisconnectedAsset, TEXT("child"));
	AddBody(*DisconnectedAsset, TEXT("orphan"));
	AddConstraint(*DisconnectedAsset, TEXT("joint"), Root, Child, EAngularConstraintMotion::ACM_Free, 0.0f);
	Error.Reset();
	TestFalse(TEXT("disconnected body graph fails"), ReflectPhysicsAssetTopology(DisconnectedAsset, Topology, Error));
	TestFalse(TEXT("disconnected graph reports a diagnostic"), Error.IsEmpty());

	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-TOPO-004: ambiguous and disconnected structures fail loudly"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTopologyReflectorCanonicalOrderTest,
	"UERL.Unit.Robot.TopologyReflector.AC_UE_UNIT_ROBOT_TOPO_005.CanonicalOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTopologyReflectorCanonicalOrderTest::RunTest(const FString& Parameters)
{
	UPhysicsAsset* First = NewObject<UPhysicsAsset>(GetTransientPackageAsObject());
	const FName FirstRoot = AddBody(*First, TEXT("root"), PhysType_Kinematic);
	const FName FirstZeta = AddBody(*First, TEXT("zeta"));
	const FName FirstAlpha = AddBody(*First, TEXT("alpha"));
	AddConstraint(*First, TEXT("root_to_zeta"), FirstRoot, FirstZeta, EAngularConstraintMotion::ACM_Free, 0.0f);
	AddConstraint(*First, TEXT("root_to_alpha"), FirstRoot, FirstAlpha, EAngularConstraintMotion::ACM_Free, 0.0f);

	UPhysicsAsset* Second = NewObject<UPhysicsAsset>(GetTransientPackageAsObject());
	const FName SecondAlpha = AddBody(*Second, TEXT("alpha"));
	const FName SecondRoot = AddBody(*Second, TEXT("root"), PhysType_Kinematic);
	const FName SecondZeta = AddBody(*Second, TEXT("zeta"));
	AddConstraint(*Second, TEXT("root_to_alpha"), SecondRoot, SecondAlpha, EAngularConstraintMotion::ACM_Free, 0.0f);
	AddConstraint(*Second, TEXT("root_to_zeta"), SecondRoot, SecondZeta, EAngularConstraintMotion::ACM_Free, 0.0f);

	FUERLRobotTopology FirstTopology;
	FUERLRobotTopology SecondTopology;
	FString Error;
	TestTrue(TEXT("first shuffled topology reflects"), ReflectPhysicsAssetTopology(First, FirstTopology, Error));
	TestTrue(TEXT("second shuffled topology reflects"), ReflectPhysicsAssetTopology(Second, SecondTopology, Error));
	TestEqual(TEXT("canonical bodies are root-first with sorted children"),
		FirstTopology.BodyNames, TArray<FName>({ TEXT("root"), TEXT("alpha"), TEXT("zeta") }));
	TestEqual(TEXT("body order ignores PhysicsAsset array order"), FirstTopology.BodyNames, SecondTopology.BodyNames);
	TestEqual(TEXT("first canonical joint is alpha"), FirstTopology.Joints[0].Name, FName(TEXT("root_to_alpha")));
	TestEqual(TEXT("joint order ignores PhysicsAsset array order"),
		FirstTopology.Joints[0].Name, SecondTopology.Joints[0].Name);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-TOPO-005: topology order ignores PhysicsAsset array order"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTopologyReflectorCartPoleAssetTest,
	"UERL.Integration.Robot.TopologyReflector.CartPoleAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTopologyReflectorCartPoleAssetTest::RunTest(const FString& Parameters)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(
		nullptr, TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole"));
	TestNotNull(TEXT("CartPole SkeletalMesh loads"), Mesh);
	if (!Mesh)
	{
		return false;
	}

	UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
	TestNotNull(TEXT("CartPole SkeletalMesh has a PhysicsAsset"), PhysicsAsset);
	if (!PhysicsAsset)
	{
		return false;
	}

	FUERLRobotTopology Topology;
	FString Error;
	TestTrue(TEXT("CartPole asset topology reflects"), FUERLTopologyReflector::ReflectSkeletalMesh(Mesh, Topology, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
		return false;
	}

	const int32 BaseIndex = Topology.BodyNames.IndexOfByKey(FName(TEXT("base")));
	const int32 CartIndex = Topology.BodyNames.IndexOfByKey(FName(TEXT("cart")));
	const int32 PoleIndex = Topology.BodyNames.IndexOfByKey(FName(TEXT("pole")));
	TestEqual(TEXT("CartPole has three bodies"), Topology.BodyNames.Num(), 3);
	TestEqual(TEXT("CartPole root is base"), Topology.RootBodyIndex, BaseIndex);
	TestTrue(TEXT("CartPole has a fixed base"), Topology.bFixedBase);
	TestTrue(TEXT("CartPole body names are present"), BaseIndex != INDEX_NONE && CartIndex != INDEX_NONE && PoleIndex != INDEX_NONE);
	if (Topology.Joints.Num() != 2 || BaseIndex == INDEX_NONE || CartIndex == INDEX_NONE || PoleIndex == INDEX_NONE)
	{
		return false;
	}

	bool bCartJointFound = false;
	bool bPoleJointFound = false;
	for (const FUERLJointTopology& Joint : Topology.Joints)
	{
		if (Joint.ParentBodyIndex == BaseIndex && Joint.ChildBodyIndex == CartIndex)
		{
			bCartJointFound = true;
			TestEqual(TEXT("CartPole cart coordinate is prismatic"), Joint.Coordinate, EUERLJointCoordinate::LinearX);
			TestEqual(TEXT("CartPole cart coordinate type is prismatic"), Joint.CoordinateType, EUERLJointCoordinateType::Prismatic);
		}
		if (Joint.ParentBodyIndex == CartIndex && Joint.ChildBodyIndex == PoleIndex)
		{
			bPoleJointFound = true;
			TestEqual(TEXT("CartPole pole coordinate is revolute"), Joint.Coordinate, EUERLJointCoordinate::Twist);
			TestEqual(TEXT("CartPole pole coordinate type is revolute"), Joint.CoordinateType, EUERLJointCoordinateType::Revolute);
		}
	}
	TestTrue(TEXT("CartPole has base-to-cart joint"), bCartJointFound);
	TestTrue(TEXT("CartPole has cart-to-pole joint"), bPoleJointFound);
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-ROBOT-TOPO-005: reflects the real CartPole PhysicsAsset"));
	return true;
}

#endif

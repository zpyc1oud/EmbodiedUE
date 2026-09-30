#include "Misc/AutomationTest.h"

#include "UERLRobotObservationPlan.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FUERLFieldDescriptor ObservationField(
		const TCHAR* Name,
		EUERLObservationType Type,
		const TCHAR* BodyName,
		int32 BodyIndex,
		int32 JointIndex,
		int32 Width,
		const TCHAR* JointName = nullptr)
	{
		FUERLFieldDescriptor Field;
		Field.Name = Name;
		Field.Shape = Width == 1 ? TArray<int32>{} : TArray<int32>{ Width };
		Field.Unit = Type == EUERLObservationType::Contact ? TEXT("fraction")
			: Type == EUERLObservationType::ContactForce ? TEXT("N")
			: Type == EUERLObservationType::GroundClearance ? TEXT("m")
			: Type == EUERLObservationType::TerrainHeight ? TEXT("m")
			: Type == EUERLObservationType::BodyPose ? TEXT("m,quat_xyzw")
			: Type == EUERLObservationType::BodyLinearVelocity ? TEXT("m/s")
			: Type == EUERLObservationType::BodyAngularVelocity ? TEXT("rad/s")
			: Type == EUERLObservationType::JointPosition
				? (BodyIndex == 1 ? TEXT("m") : TEXT("rad"))
				: (BodyIndex == 1 ? TEXT("m/s") : TEXT("rad/s"));
		Field.CoordinateFrame = Type == EUERLObservationType::Contact
			|| Type == EUERLObservationType::ContactForce ? TEXT("coordinate-free")
			: Type == EUERLObservationType::JointPosition || Type == EUERLObservationType::JointVelocity
				? TEXT("constraint") : TEXT("slot/local");
		Field.Semantic = UERLObservationTypeName(Type);
		Field.Source = TEXT("uerl.robot");
		Field.Width = Width;
		Field.Observation.Type = Type;
		Field.Observation.BodyName = BodyName;
		Field.Observation.BodyIndex = BodyIndex;
		Field.Observation.JointIndex = JointIndex;
		if (JointIndex != INDEX_NONE)
		{
			if (JointName != nullptr)
			{
				Field.Observation.JointName = JointName;
			}
			else if (JointIndex == 0)
			{
				Field.Observation.JointName = TEXT("slider");
			}
			else if (JointIndex == 1)
			{
				Field.Observation.JointName = TEXT("pole_joint");
			}
			else
			{
				// Out-of-range indices still need a non-None name for IsValid().
				Field.Observation.JointName = TEXT("pole_joint");
			}
		}
		return Field;
	}

	FUERLRobotTopology TestTopology()
	{
		FUERLRobotTopology Topology;
		Topology.BodyNames = { TEXT("world"), TEXT("cart"), TEXT("pole") };
		Topology.BodyMotionTypes = { EUERLBodyMotionType::Kinematic, EUERLBodyMotionType::Simulated, EUERLBodyMotionType::Simulated };
		Topology.RootBodyIndex = 0;
		Topology.bFixedBase = true;
		FUERLJointTopology Slider;
		Slider.Name = TEXT("slider");
		Slider.ParentBodyIndex = 0;
		Slider.ChildBodyIndex = 1;
		Slider.DegreesOfFreedom = 1;
		Slider.Coordinate = EUERLJointCoordinate::LinearX;
		Slider.CoordinateType = EUERLJointCoordinateType::Prismatic;
		Topology.Joints.Add(Slider);
		FUERLJointTopology Pole;
		Pole.Name = TEXT("pole_joint");
		Pole.ParentBodyIndex = 1;
		Pole.ChildBodyIndex = 2;
		Pole.DegreesOfFreedom = 1;
		Pole.Coordinate = EUERLJointCoordinate::Twist;
		Pole.CoordinateType = EUERLJointCoordinateType::Revolute;
		Topology.Joints.Add(Pole);
		return Topology;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLRobotObservationPlanValidTest,
	"UERL.Unit.Robot.ObservationPlan.ValidOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLRobotObservationPlanValidTest::RunTest(const FString& Parameters)
{
	const TArray<FUERLFieldDescriptor> Fields = {
		ObservationField(TEXT("robot.body.world.body_pose"), EUERLObservationType::BodyPose, TEXT("world"), 0, INDEX_NONE, 7),
		ObservationField(TEXT("robot.joint.pole_joint.joint_position"), EUERLObservationType::JointPosition, TEXT("pole"), 2, 1, 1),
		ObservationField(TEXT("robot.joint.pole_joint.joint_velocity"), EUERLObservationType::JointVelocity, TEXT("pole"), 2, 1, 1),
		ObservationField(TEXT("robot.body.cart.body_linear_velocity"), EUERLObservationType::BodyLinearVelocity, TEXT("cart"), 1, INDEX_NONE, 3),
		ObservationField(TEXT("robot.body.pole.body_angular_velocity"), EUERLObservationType::BodyAngularVelocity, TEXT("pole"), 2, INDEX_NONE, 3),
		ObservationField(TEXT("robot.body.pole.ground_clearance"), EUERLObservationType::GroundClearance, TEXT("pole"), 2, INDEX_NONE, 1),
		ObservationField(TEXT("robot.body.pole.contact"), EUERLObservationType::Contact, TEXT("pole"), 2, INDEX_NONE, 1),
		ObservationField(TEXT("robot.body.pole.contact_force"), EUERLObservationType::ContactForce, TEXT("pole"), 2, INDEX_NONE, 1),
		ObservationField(TEXT("robot.body.world.terrain_height"), EUERLObservationType::TerrainHeight, TEXT("world"), 0, INDEX_NONE, 35),
	};
	TArray<FUERLRobotObservationPlanEntry> Plan;
	FString Error;

	TestTrue(TEXT("valid observation subset compiles"), CompileRobotObservationPlan(TestTopology(), Fields, Plan, Error));
	TestEqual(TEXT("plan preserves selected order"), Plan.Num(), 9);
	if (Plan.Num() == 9)
	{
		TestEqual(TEXT("first field is body pose"), Plan[0].FieldName, FName(TEXT("robot.body.world.body_pose")));
		TestEqual(TEXT("joint position body index"), Plan[1].BodyIndex, 2);
		TestEqual(TEXT("joint position joint index"), Plan[1].JointIndex, 1);
		TestEqual(TEXT("joint velocity joint index"), Plan[2].JointIndex, 1);
		TestEqual(TEXT("linear velocity width"), Plan[3].Width, 3);
		TestEqual(TEXT("angular velocity width"), Plan[4].Width, 3);
		TestEqual(TEXT("ground clearance width"), Plan[5].Width, 1);
		TestEqual(TEXT("contact body index"), Plan[6].BodyIndex, 2);
		TestEqual(TEXT("contact width"), Plan[6].Width, 1);
		TestEqual(TEXT("contact-force body index"), Plan[7].BodyIndex, 2);
		TestEqual(TEXT("contact-force width"), Plan[7].Width, 1);
		TestEqual(TEXT("terrain-height scan width"), Plan[8].Width, 35);
	}
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-PLAN-001: selected observation subset compiles in stable order"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLRobotObservationPlanSelectedFilterTest,
	"UERL.Unit.Robot.ObservationPlan.SelectedFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLRobotObservationPlanSelectedFilterTest::RunTest(const FString& Parameters)
{
	const TArray<FUERLFieldDescriptor> Available = {
		ObservationField(TEXT("robot.body.world.body_pose"), EUERLObservationType::BodyPose, TEXT("world"), 0, INDEX_NONE, 7),
		ObservationField(TEXT("robot.body.cart.body_pose"), EUERLObservationType::BodyPose, TEXT("cart"), 1, INDEX_NONE, 7),
	};
	FUERLFieldDescriptor EnvironmentField;
	EnvironmentField.Name = TEXT("environment.height");
	const TArray<FUERLFieldDescriptor> Selected = { EnvironmentField, Available[1], Available[0] };
	TArray<FUERLFieldDescriptor> Owned;

	SelectRobotObservationFields(Available, Selected, Owned);
	TestEqual(TEXT("only Robot fields remain selected"), Owned.Num(), 2);
	if (Owned.Num() == 2)
	{
		TestEqual(TEXT("Robot selection preserves selected order"), Owned[0].Name, Available[1].Name);
		TestEqual(TEXT("Robot selection preserves the second selected field"), Owned[1].Name, Available[0].Name);
	}
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-PLAN-005: selected State filtering preserves order and ownership"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLRobotObservationPlanEmptySelectionTest,
	"UERL.Unit.Robot.ObservationPlan.EmptySelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLRobotObservationPlanEmptySelectionTest::RunTest(const FString& Parameters)
{
	TArray<FUERLRobotObservationPlanEntry> Plan;
	FString Error;

	TestTrue(TEXT("empty selected State compiles"),
		CompileRobotObservationPlan(TestTopology(), {}, Plan, Error));
	TestEqual(TEXT("empty selected State produces no collection entries"), Plan.Num(), 0);
	TestTrue(TEXT("empty selected State has no diagnostic"), Error.IsEmpty());
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-PLAN-004: unselected robot State produces an empty plan"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLRobotObservationPlanRejectsInvalidBindingsTest,
	"UERL.Unit.Robot.ObservationPlan.RejectsInvalidBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLRobotObservationPlanRejectsInvalidBindingsTest::RunTest(const FString& Parameters)
{
	FString Error;
	TArray<FUERLRobotObservationPlanEntry> Plan;

	FUERLFieldDescriptor WrongBody = ObservationField(
		TEXT("robot.body.pole.body_pose"), EUERLObservationType::BodyPose, TEXT("cart"), 2, INDEX_NONE, 7);
	TestFalse(TEXT("body index/name mismatch is rejected"),
		CompileRobotObservationPlan(TestTopology(), { WrongBody }, Plan, Error));
	TestEqual(TEXT("failed plan is cleared"), Plan.Num(), 0);

	FUERLFieldDescriptor WrongJointIndex = ObservationField(
		TEXT("robot.joint.pole_joint.joint_position"), EUERLObservationType::JointPosition, TEXT("pole"), 2, 99, 1);
	TestFalse(TEXT("joint index out of range is rejected"),
		CompileRobotObservationPlan(TestTopology(), { WrongJointIndex }, Plan, Error));

	FUERLFieldDescriptor WrongJoint = ObservationField(
		TEXT("robot.joint.pole_joint.joint_position"), EUERLObservationType::JointPosition, TEXT("cart"), 1, 1, 1);
	TestFalse(TEXT("joint child-body mismatch is rejected"),
		CompileRobotObservationPlan(TestTopology(), { WrongJoint }, Plan, Error));

	FUERLFieldDescriptor WrongJointName = ObservationField(
		TEXT("robot.joint.pole_joint.joint_position"), EUERLObservationType::JointPosition, TEXT("pole"), 2, 1, 1,
		TEXT("slider"));
	TestFalse(TEXT("joint index/name mismatch is rejected"),
		CompileRobotObservationPlan(TestTopology(), { WrongJointName }, Plan, Error));

	FUERLFieldDescriptor WrongWidth = ObservationField(
		TEXT("robot.body.cart.body_linear_velocity"), EUERLObservationType::BodyLinearVelocity, TEXT("cart"), 1, INDEX_NONE, 1);
	TestFalse(TEXT("vector width mismatch is rejected"),
		CompileRobotObservationPlan(TestTopology(), { WrongWidth }, Plan, Error));

	FUERLFieldDescriptor Contact = ObservationField(
		TEXT("robot.body.pole.contact"), EUERLObservationType::Contact, TEXT("pole"), 2, INDEX_NONE, 1);
	TestTrue(TEXT("contact observation is a valid scalar plan entry"),
		CompileRobotObservationPlan(TestTopology(), { Contact }, Plan, Error));
	TestEqual(TEXT("contact plan contains one entry"), Plan.Num(), 1);

	FUERLFieldDescriptor 	WrongContactMetadata = Contact;
	WrongContactMetadata.Unit = TEXT("m");
	TestFalse(TEXT("contact unit mismatch is rejected"),
		CompileRobotObservationPlan(TestTopology(), { WrongContactMetadata }, Plan, Error));
	FUERLFieldDescriptor ContactForce = ObservationField(
		TEXT("robot.body.pole.contact_force"), EUERLObservationType::ContactForce, TEXT("pole"), 2, INDEX_NONE, 1);
	TestTrue(TEXT("contact-force observation is a valid scalar plan entry"),
		CompileRobotObservationPlan(TestTopology(), { ContactForce }, Plan, Error));
	ContactForce.Unit = TEXT("fraction");
	TestFalse(TEXT("contact-force unit mismatch is rejected"),
		CompileRobotObservationPlan(TestTopology(), { ContactForce }, Plan, Error));
	WrongContactMetadata = Contact;
	WrongContactMetadata.Shape = { 1 };
	TestFalse(TEXT("contact vector shape is rejected"),
		CompileRobotObservationPlan(TestTopology(), { WrongContactMetadata }, Plan, Error));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-PLAN-002: topology, width, and contact metadata mismatches fail loudly"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLRobotObservationPlanRejectsDuplicateTest,
	"UERL.Unit.Robot.ObservationPlan.RejectsDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLRobotObservationPlanRejectsDuplicateTest::RunTest(const FString& Parameters)
{
	const FUERLFieldDescriptor Field = ObservationField(
		TEXT("robot.joint.pole_joint.joint_position"), EUERLObservationType::JointPosition, TEXT("pole"), 2, 1, 1);
	TArray<FUERLRobotObservationPlanEntry> Plan;
	FString Error;

	TestFalse(TEXT("duplicate selected field is rejected"),
		CompileRobotObservationPlan(TestTopology(), { Field, Field }, Plan, Error));
	TestEqual(TEXT("duplicate failure clears plan"), Plan.Num(), 0);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-PLAN-003: duplicate observation fields fail loudly"));
	return true;
}

#endif

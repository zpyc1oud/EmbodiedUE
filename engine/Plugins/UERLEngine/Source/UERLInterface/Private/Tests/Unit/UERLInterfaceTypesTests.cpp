#include "Misc/AutomationTest.h"

#include "UERLInterfaceTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLInterfaceProviderConfigTest,
	"UERL.Unit.Interface.ProviderConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLInterfaceProviderConfigTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("empty asset path keeps provider compatibility"), FUERLProviderConfig::IsValidAssetPath(TEXT("")));
	TestTrue(TEXT("UE object path is accepted"), FUERLProviderConfig::IsValidAssetPath(TEXT("/Game/Robots/CartPole")));
	TestFalse(TEXT("relative path is rejected"), FUERLProviderConfig::IsValidAssetPath(TEXT("Game/Robots/CartPole")));
	TestFalse(TEXT("Windows path is rejected"), FUERLProviderConfig::IsValidAssetPath(TEXT("C:\\Robots\\CartPole")));

	const FUERLDistributionConfig Constant = FUERLDistributionConfig::Constant(0.0, TEXT("reset.position"));
	TestTrue(TEXT("constant distribution is valid"), Constant.IsValid());
	TestTrue(TEXT("uniform distribution is valid"), FUERLDistributionConfig::Uniform(-1.0, 1.0, TEXT("reset.angle")).IsValid());
	TestFalse(TEXT("uniform distribution requires a stream"), FUERLDistributionConfig::Uniform(-1.0, 1.0, NAME_None).IsValid());
	TestFalse(TEXT("distribution bounds must be ordered"), FUERLDistributionConfig::Uniform(1.0, -1.0, TEXT("reset.angle")).IsValid());
	AddInfo(TEXT("[VERIFY] Interface provider configuration owns asset-path and distribution validation"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLInterfaceObservationDescriptorTest,
	"UERL.Unit.Interface.ObservationDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLInterfaceObservationDescriptorTest::RunTest(const FString& Parameters)
{
	FUERLFieldDescriptor Field;
	Field.Name = TEXT("robot.body.2.body_linear_velocity");
	Field.Shape = { 3 };
	Field.Unit = TEXT("m/s");
	Field.CoordinateFrame = TEXT("slot/world");
	Field.Semantic = TEXT("body_linear_velocity");
	Field.Source = TEXT("uerl.robot");
	Field.Width = 3;
	Field.Observation.Type = EUERLObservationType::BodyLinearVelocity;
	Field.Observation.BodyName = TEXT("pole");
	Field.Observation.BodyIndex = 2;

	TestTrue(TEXT("valid observation descriptor is accepted"), Field.IsValid());

	FUERLObservationBinding ValidJoint;
	ValidJoint.Type = EUERLObservationType::JointPosition;
	ValidJoint.BodyName = TEXT("pole");
	ValidJoint.BodyIndex = 2;
	ValidJoint.JointName = TEXT("pole_joint");
	ValidJoint.JointIndex = 1;
	TestTrue(TEXT("valid joint observation requires name and index"), ValidJoint.IsValid());

	FUERLObservationBinding MissingJointIndex;
	MissingJointIndex.Type = EUERLObservationType::JointPosition;
	MissingJointIndex.BodyName = TEXT("pole");
	MissingJointIndex.BodyIndex = 2;
	MissingJointIndex.JointName = TEXT("pole_joint");
	TestFalse(TEXT("joint observation requires a joint index"), MissingJointIndex.IsValid());

	FUERLObservationBinding MissingJointName;
	MissingJointName.Type = EUERLObservationType::JointPosition;
	MissingJointName.BodyName = TEXT("pole");
	MissingJointName.BodyIndex = 2;
	MissingJointName.JointIndex = 1;
	TestFalse(TEXT("joint observation requires a joint name"), MissingJointName.IsValid());

	FUERLObservationBinding ResidualMetadata;
	ResidualMetadata.BodyName = TEXT("pole");
	ResidualMetadata.BodyIndex = 2;
	TestFalse(TEXT("None with residual metadata is rejected"), ResidualMetadata.IsValid());
	AddInfo(TEXT("[VERIFY] Interface observation descriptor owns binding invariants"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLInterfaceResetBindingTest,
	"UERL.Unit.Interface.ResetBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLInterfaceResetBindingTest::RunTest(const FString& Parameters)
{
	FUERLResetBinding RootPose;
	RootPose.Index = 0;
	RootPose.Name = TEXT("root_pose:0");
	RootPose.TargetType = TEXT("root_pose");
	RootPose.BodyIndex = 0;
	RootPose.ComponentIndex = 0;
	RootPose.Unit = TEXT("m,quat_xyzw");
	TestTrue(TEXT("root pose component binding is valid"), RootPose.IsValid());

	RootPose.ComponentIndex = 7;
	TestFalse(TEXT("root pose rejects an out-of-range component"), RootPose.IsValid());

	FUERLResetBinding Joint;
	Joint.Index = 0;
	Joint.Name = TEXT("joint_position:cart");
	Joint.TargetType = TEXT("joint_position");
	Joint.JointIndex = 0;
	Joint.Unit = TEXT("m");
	Joint.ComponentIndex = 1;
	TestFalse(TEXT("joint reset remains scalar"), Joint.IsValid());
	AddInfo(TEXT("[VERIFY] Interface reset bindings own scalar and root-vector component invariants"));
	return true;
}

#endif

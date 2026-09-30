#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#include "UERLGenericRobotProvider.h"
#include "UERLRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotFactoryRegistrationTest,
	"UERL.Unit.Robot.GenericFactory.RegistrationAndConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotFactoryRegistrationTest::RunTest(const FString& Parameters)
{
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLRobot"));
	FString Error;
	const TSharedPtr<IUERLRobotFactory> RegisteredFactory = FUERLRobotRegistry::Get().Resolve(
		UERLGenericRobot::RobotId, Error);
	TestTrue(TEXT("generic SkeletalMesh factory is registered"), RegisteredFactory.IsValid());
	if (!RegisteredFactory.IsValid())
	{
		return false;
	}
	const TSharedRef<IUERLRobotFactory> Factory = UERLGenericRobot::MakeFactory();
	const FUERLRobotDescriptor& Descriptor = Factory->Describe();
	TestEqual(TEXT("generic provider has no Action before indexed config"), Descriptor.ActionFields.Num(), 0);

	FUERLProviderConfig Effective;
	FUERLProviderConfig Empty;
	TestFalse(TEXT("empty asset path is rejected"), Factory->ValidateConfig(Empty, Effective, Error));
	TestFalse(TEXT("empty path reports a diagnostic"), Error.IsEmpty());

	FUERLProviderConfig Relative;
	Relative.AssetPath = TEXT("Game/MVP0/CartPole");
	Error.Reset();
	TestFalse(TEXT("relative asset path is rejected by the factory"), Factory->ValidateConfig(Relative, Effective, Error));

	FUERLProviderConfig Missing;
	Missing.AssetPath = TEXT("/Game/MVP0/MissingSkeletalMesh.MissingSkeletalMesh");
	Error.Reset();
	TestFalse(TEXT("missing SkeletalMesh asset is rejected"), Factory->ValidateConfig(Missing, Effective, Error));
	TestFalse(TEXT("missing asset reports a diagnostic"), Error.IsEmpty());

	FUERLProviderConfig StaticMesh;
	StaticMesh.AssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	Error.Reset();
	TestFalse(TEXT("non-SkeletalMesh asset is rejected"), Factory->ValidateConfig(StaticMesh, Effective, Error));
	TestFalse(TEXT("non-SkeletalMesh reports a diagnostic"), Error.IsEmpty());

	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-GENERIC-001: generic factory registration and asset failures are explicit"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotStateDescriptorTest,
	"UERL.Unit.Robot.GenericFactory.StateDescriptors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotStateDescriptorTest::RunTest(const FString& Parameters)
{
	FUERLRobotTopology Topology;
	Topology.BodyNames = { TEXT("world"), TEXT("cart"), TEXT("pole") };
	Topology.RootBodyIndex = 0;
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

	TArray<FUERLFieldDescriptor> Fields;
	FString Error;
	TestTrue(TEXT("synthetic topology publishes state descriptors"),
		UERLGenericRobot::BuildGenericRobotStateFields(Topology, Fields, Error));
	TestEqual(TEXT("six body quantities, root terrain scan, and two scalar joint pairs"), Fields.Num(), 23);
	for (const FUERLFieldDescriptor& Field : Fields)
	{
		TestTrue(TEXT("published descriptor is valid"), Field.IsValid());
	}
	bool bHasPolePosition = false;
	bool bHasSliderPosition = false;
	bool bHasPoleContact = false;
	bool bHasPoleContactForce = false;
	bool bHasPoleGroundClearance = false;
	bool bHasRootTerrainHeight = false;
	TestEqual(TEXT("terrain-height scan has seven forward samples"), UERLTerrainHeightScanForwardCount, 7);
	TestEqual(TEXT("terrain-height scan has five lateral samples"), UERLTerrainHeightScanLateralCount, 5);
	TestEqual(TEXT("terrain-height scan publishes 35 samples"), UERLTerrainHeightScanWidth, 35);
	for (const FUERLFieldDescriptor& Field : Fields)
	{
		bHasPolePosition |= Field.Name == FName(TEXT("robot.joint.pole_joint.joint_position"))
			&& Field.Unit == TEXT("rad");
		bHasSliderPosition |= Field.Name == FName(TEXT("robot.joint.slider.joint_position"))
			&& Field.Unit == TEXT("m");
		bHasPoleContact |= Field.Name == FName(TEXT("robot.body.pole.contact"));
		bHasPoleContactForce |= Field.Name == FName(TEXT("robot.body.pole.contact_force"))
			&& Field.Unit == TEXT("N")
			&& Field.CoordinateFrame == TEXT("coordinate-free")
			&& Field.Semantic == TEXT("contact_force");
		bHasPoleGroundClearance |= Field.Name == FName(TEXT("robot.body.pole.ground_clearance"))
			&& Field.Unit == TEXT("m");
		bHasRootTerrainHeight |= Field.Name == FName(TEXT("robot.body.world.terrain_height"))
			&& Field.Width == UERLTerrainHeightScanWidth
			&& Field.Shape == TArray<int32>{ UERLTerrainHeightScanWidth }
			&& Field.Semantic == TEXT("terrain_height");
	}
	TestTrue(TEXT("angular scalar joint position is published in radians"), bHasPolePosition);
	TestTrue(TEXT("linear scalar joint position is published in metres"), bHasSliderPosition);
	TestTrue(TEXT("body contact descriptors are published"), bHasPoleContact);
	TestTrue(TEXT("body contact-force descriptors are published in newtons"), bHasPoleContactForce);
	TestTrue(TEXT("body ground-clearance descriptors are published in metres"), bHasPoleGroundClearance);
	TestTrue(TEXT("root terrain-height scan descriptor is published"), bHasRootTerrainHeight);
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ROBOT-GENERIC-002: body, contact, and angular scalar State descriptors are explicit"));
	return true;
}

#endif

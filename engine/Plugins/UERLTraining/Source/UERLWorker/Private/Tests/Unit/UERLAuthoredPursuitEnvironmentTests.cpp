#include "Misc/AutomationTest.h"

#include "UERLAuthoredPursuitEnvironment.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLAuthoredPursuitEnvironmentDescriptorTest,
	"UERL.Unit.Worker.AuthoredPursuit.DescriptorAndConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLAuthoredPursuitEnvironmentDescriptorTest::RunTest(const FString& Parameters)
{
	TSharedRef<IUERLEnvironmentFactory> Factory = UERLAuthoredPursuit::MakeFactory();
	const FUERLEnvironmentDescriptor& Descriptor = Factory->Describe();
	TestEqual(TEXT("authored pursuit uses SharedWorld collision"),
		Descriptor.CollisionScope, EUERLEnvironmentCollisionScope::SharedWorld);
	TestEqual(TEXT("authored pursuit publishes one target field"), Descriptor.StateFields.Num(), 1);
	if (Descriptor.StateFields.Num() == 1)
	{
		TestEqual(TEXT("target field name is stable"),
			Descriptor.StateFields[0].Name, UERLAuthoredPursuit::TargetPositionField);
		TestEqual(TEXT("target position is a three-vector"), Descriptor.StateFields[0].Width, 3);
	}

	FUERLProviderConfig Input;
	FUERLProviderConfig Effective;
	FString Error;
	TestTrue(TEXT("authored pursuit defaults validate"),
		Factory->ValidateConfig(Input, Effective, Error));
	TestEqual(TEXT("default trace starts ten metres above the world origin"),
		Effective.Scalars[UERLAuthoredPursuit::TraceStartZ], 10.0);
	Input.Scalars.Add(TEXT("environment.unowned"), 1.0);
	TestFalse(TEXT("unknown Environment scalar fails at the provider boundary"),
		Factory->ValidateConfig(Input, Effective, Error));
	return true;
}

#endif

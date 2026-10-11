#include "UERLInputProvider.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExampleInputPublicationTest,
	"ExampleInput.Integration.RegisteredPublication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FExampleInputPublicationTest::RunTest(const FString&)
{
	FString Error;
	FUERLInputRegistry& Registry = FUERLInputRegistry::Get();
	if (!TestTrue(TEXT("plugin startup registered factory"),
		Registry.Resolve(TEXT("example.constant"), 1, Error).IsValid())) { return false; }
	FUERLInputSpec First;
	First.Name = TEXT("first"); First.ProviderId = TEXT("example.constant");
	First.ParametersJson = TEXT("{\"value\":2.5}");
	FUERLInputSpec Second = First;
	Second.Name = TEXT("second"); Second.ParametersJson = TEXT("{\"value\":-3.25}");
	const TArray<FUERLInputSpec> Specs = {First, Second};
	TArray<FUERLInputSlotBinding> Slots;
	Slots.SetNum(2); Slots[0].Slot.SlotId = 2; Slots[1].Slot.SlotId = 7;
	FUERLCompiledInputSet Set;
	if (!TestTrue(TEXT("bind external providers"), Set.Bind(Registry, Specs, Slots, Error))) { return false; }
	if (!TestEqual(TEXT("two output descriptors"), Set.GetFields().Num(), 2)) { return false; }
	TestEqual(TEXT("first descriptor"), Set.GetFields()[0].Name, FName(TEXT("input.first.value")));
	TestEqual(TEXT("second descriptor"), Set.GetFields()[1].Name, FName(TEXT("input.second.value")));
	FUERLInputSampleStamp Stamp;
	Stamp.Sequence = 1; Stamp.SolverTimeSeconds = 0.02;
	TConstArrayView<float> Values;
	for (int32 SlotId : {2, 7})
	{
		if (!TestTrue(TEXT("sample sparse Slot"), Set.Sample(SlotId, Stamp, Values, Error))) { return false; }
		if (!TestEqual(TEXT("two scalar outputs"), Values.Num(), 2)) { return false; }
		TestEqual(TEXT("first literal output"), Values[0], 2.5f);
		TestEqual(TEXT("second distinct offset"), Values[1], -3.25f);
	}
	const TArray<int32> ResetIds = {2};
	if (!TestTrue(TEXT("reset selected Slot"), Set.Reset(ResetIds, Error))) { return false; }
	TestEqual(TEXT("selected generation"), Set.GetResetGeneration(2), int64(1));
	TestEqual(TEXT("unselected generation"), Set.GetResetGeneration(7), int64(0));
	if (!TestTrue(TEXT("other publication survives"), Set.Sample(7, Stamp, Values, Error))) { return false; }
	TestEqual(TEXT("unchanged other output"), Values[1], -3.25f);
	Stamp.ResetGeneration = 1;
	if (!TestTrue(TEXT("selected publication recovers"), Set.Sample(2, Stamp, Values, Error))) { return false; }
	TestEqual(TEXT("reset first output"), Values[0], 2.5f);
	Set.Release();
	TestFalse(TEXT("release removes binding"), Set.Sample(2, Stamp, Values, Error));
	return true;
}
#endif

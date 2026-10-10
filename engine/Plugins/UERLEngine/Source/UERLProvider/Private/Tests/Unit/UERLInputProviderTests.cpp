#include "UERLInputProvider.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UERLInputTest
{
	struct FProbe
	{
		int32 Samples = 0;
		int32 Resets = 0;
		int32 Releases = 0;
		bool bMissingValue = false;
		bool bFailBind = false;
	};
	class FProvider final : public IUERLInputProvider
	{
	public:
		explicit FProvider(FProbe& InProbe) : Probe(InProbe) {}
		bool Bind(TConstArrayView<FUERLInputSlotBinding>, FString& OutError) override
		{
			if (Probe.bFailBind) { OutError = TEXT("test bind failure"); return false; }
			return true;
		}
		bool Sample(int32 SlotId, const FUERLInputSampleStamp& Stamp,
			TArrayView<float> Values, FString&) override
		{
			++Probe.Samples;
			Values[0] = static_cast<float>(SlotId * 10 + Stamp.Sequence);
			if (!Probe.bMissingValue) { Values[1] = Values[0] + 1.0f; }
			return true;
		}
		bool Reset(TConstArrayView<int32> SlotIds, FString&) override { Probe.Resets += SlotIds.Num(); return true; }
		void Release() override { ++Probe.Releases; }
	private:
		FProbe& Probe;
	};
	class FFactory final : public IUERLInputFactory
	{
	public:
		explicit FFactory(FProbe& InProbe) : Probe(InProbe) {}
		FString GetId() const override { return TEXT("test.constant"); }
		int32 GetVersion() const override { return 1; }
		bool Validate(const FUERLInputSpec& Spec, FUERLInputSpec& OutSpec,
			TArray<FUERLFieldDescriptor>& Fields, FString&) const override
		{
			OutSpec = Spec;
			FUERLFieldDescriptor Field;
			Field.Name = FName(*FString::Printf(TEXT("input.%s.value"), *Spec.Name));
			Field.Shape = {2}; Field.Width = 2; Field.Unit = TEXT("m");
			Field.CoordinateFrame = TEXT("body"); Field.Semantic = TEXT("test_position");
			Field.Source = GetId();
			Fields.Add(Field);
			return true;
		}
		TUniquePtr<IUERLInputProvider> Create(const FUERLInputSpec&) const override
		{
			return MakeUnique<FProvider>(Probe);
		}
	private:
		FProbe& Probe;
	};
	FUERLInputSpec Spec(const TCHAR* Name)
	{
		FUERLInputSpec Result; Result.Name = Name; Result.ProviderId = TEXT("test.constant"); return Result;
	}
	TArray<FUERLInputSlotBinding> Bindings()
	{
		TArray<FUERLInputSlotBinding> Result;
		Result.SetNum(2); Result[0].Slot.SlotId = 2; Result[1].Slot.SlotId = 7; return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLInputCacheTest, "UERL.Unit.InputProvider.CacheAndSparseReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLInputCacheTest::RunTest(const FString&)
{
	using namespace UERLInputTest;
	FProbe Probe;
	FUERLInputRegistry Registry;
	FString Error;
	if (!TestTrue(TEXT("register"), Registry.RegisterFactory(MakeShared<FFactory>(Probe), Error))) { return false; }
	FUERLCompiledInputSet Set;
	const TArray<FUERLInputSpec> Specs = {Spec(TEXT("left")), Spec(TEXT("right"))};
	const TArray<FUERLInputSlotBinding> Slots = Bindings();
	if (!TestTrue(TEXT("bind"), Set.Bind(Registry, Specs, Slots, Error))) { return false; }
	FUERLInputSampleStamp Stamp; Stamp.Sequence = 1; Stamp.SolverTimeSeconds = 0.02;
	TConstArrayView<float> Values;
	if (!TestTrue(TEXT("first sample"), Set.Sample(2, Stamp, Values, Error))) { return false; }
	if (!TestEqual(TEXT("two two-wide fields"), Values.Num(), 4)) { return false; }
	TestEqual(TEXT("first field scalar"), Values[0], 21.0f);
	TestEqual(TEXT("first field second scalar"), Values[1], 22.0f);
	TestEqual(TEXT("second field offset"), Values[2], 21.0f);
	TestTrue(TEXT("repeat read"), Set.Sample(2, Stamp, Values, Error));
	TestEqual(TEXT("only two provider samples"), Probe.Samples, 2);
	if (!TestTrue(TEXT("other stable Slot"), Set.Sample(7, Stamp, Values, Error))) { return false; }
	TestEqual(TEXT("stable Slot identity"), Values[0], 71.0f);
	const TArray<int32> ResetIds = {2};
	TestTrue(TEXT("sparse reset"), Set.Reset(ResetIds, Error));
	TestEqual(TEXT("selected generation"), Set.GetResetGeneration(2), static_cast<int64>(1));
	TestEqual(TEXT("unselected generation"), Set.GetResetGeneration(7), static_cast<int64>(0));
	TestTrue(TEXT("unselected cache remains readable"), Set.Sample(7, Stamp, Values, Error));
	TestEqual(TEXT("unselected did not resample"), Probe.Samples, 4);
	TestFalse(TEXT("old reset generation rejected"), Set.Sample(2, Stamp, Values, Error));
	Stamp.ResetGeneration = 1;
	TestTrue(TEXT("selected bootstrap"), Set.Sample(2, Stamp, Values, Error));
	TestEqual(TEXT("selected resampled both inputs"), Probe.Samples, 6);
	TestEqual(TEXT("two provider reset callbacks"), Probe.Resets, 2);
	Set.Release(); Set.Release();
	TestEqual(TEXT("release exactly once per instance"), Probe.Releases, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLInputFaultTest, "UERL.Unit.InputProvider.FaultAndClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLInputFaultTest::RunTest(const FString&)
{
	using namespace UERLInputTest;
	FProbe Probe; FUERLInputRegistry Registry; FString Error;
	Registry.RegisterFactory(MakeShared<FFactory>(Probe), Error);
	FUERLCompiledInputSet Set;
	const TArray<FUERLInputSpec> Specs = {Spec(TEXT("probe"))};
	const TArray<FUERLInputSlotBinding> Slots = Bindings();
	if (!TestTrue(TEXT("bind"), Set.Bind(Registry, Specs, Slots, Error))) { return false; }
	FUERLInputSampleStamp Stamp; Stamp.Sequence = 3; Stamp.SolverTimeSeconds = 0.06;
	TConstArrayView<float> Values;
	TestTrue(TEXT("sample"), Set.Sample(2, Stamp, Values, Error));
	Stamp.Sequence = 2; Stamp.SolverTimeSeconds = 0.04;
	TestFalse(TEXT("stale clock rejected"), Set.Sample(2, Stamp, Values, Error));
	TestEqual(TEXT("no stale publication"), Values.Num(), 0);
	Stamp.Sequence = 4; Stamp.SolverTimeSeconds = 0.08;
	Probe.bMissingValue = true;
	TestFalse(TEXT("unwritten scalar faults"), Set.Sample(2, Stamp, Values, Error));
	TestTrue(TEXT("actionable provider diagnostic"), Error.Contains(TEXT("probe")) && Error.Contains(TEXT("Slot 2")));
	Probe.bMissingValue = false;
	TestFalse(TEXT("fault requires reset"), Set.Sample(2, Stamp, Values, Error));
	TestEqual(TEXT("fault retry did not sample"), Probe.Samples, 2);
	const TArray<int32> ResetIds = {2};
	TestTrue(TEXT("recover"), Set.Reset(ResetIds, Error));
	Stamp.ResetGeneration = 1;
	TestTrue(TEXT("recovered publication"), Set.Sample(2, Stamp, Values, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLInputBindingTest, "UERL.Unit.InputProvider.BindingFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLInputBindingTest::RunTest(const FString&)
{
	using namespace UERLInputTest;
	FProbe Probe; FUERLInputRegistry Registry; FString Error;
	const TSharedRef<FFactory> Factory = MakeShared<FFactory>(Probe);
	TestTrue(TEXT("register"), Registry.RegisterFactory(Factory, Error));
	TestFalse(TEXT("duplicate registry key"), Registry.RegisterFactory(Factory, Error));
	TestFalse(TEXT("unsupported version"), Registry.Resolve(TEXT("test.constant"), 2, Error).IsValid());
	FUERLCompiledInputSet Set;
	TArray<FUERLInputSpec> Specs = {Spec(TEXT("probe"))};
	const TArray<FUERLInputSlotBinding> Slots = Bindings();
	Specs[0].SceneBindings.Add(TEXT("ground"));
	TestFalse(TEXT("missing logical ground"), Set.Bind(Registry, Specs, Slots, Error));
	TestEqual(TEXT("no instances allocated for invalid binding"), Probe.Releases, 0);
	Specs[0].SceneBindings.Reset(); Probe.bFailBind = true;
	TestFalse(TEXT("partial provider bind"), Set.Bind(Registry, Specs, Slots, Error));
	TestEqual(TEXT("partial instance released"), Probe.Releases, 1);
	TestEqual(TEXT("failed binding removes fields"), Set.GetFields().Num(), 0);
	Probe.bFailBind = false;
	TestTrue(TEXT("retry binding"), Set.Bind(Registry, Specs, Slots, Error));
	Set.Release();
	Registry.UnregisterFactory(TEXT("test.constant"), 1);
	TestFalse(TEXT("unregistered factory unavailable"), Registry.Resolve(TEXT("test.constant"), 1, Error).IsValid());
	return true;
}
#endif

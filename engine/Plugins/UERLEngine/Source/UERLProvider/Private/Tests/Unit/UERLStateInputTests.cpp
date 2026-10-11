#include "UERLStateInput.h"
#include "Misc/AutomationTest.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace UERLStateInputTest
{
	TArray<FUERLFieldDescriptor> Catalog()
	{
		FUERLFieldDescriptor Velocity;
		Velocity.Name = TEXT("body_velocity"); Velocity.Shape = {2, 3}; Velocity.Width = 6;
		Velocity.Unit = TEXT("m/s"); Velocity.CoordinateFrame = TEXT("ground");
		Velocity.Semantic = TEXT("linear_velocity"); Velocity.Source = TEXT("robot");
		FUERLFieldDescriptor Support;
		Support.Name = TEXT("support"); Support.Shape = {2}; Support.Width = 2;
		Support.Unit = TEXT("1"); Support.CoordinateFrame = TEXT("world");
		Support.Semantic = TEXT("geometric_support"); Support.Source = TEXT("robot");
		return {Velocity, Support};
	}
	FUERLInputSpec Spec()
	{
		FUERLInputSpec Result;
		Result.Name = TEXT("state"); Result.ProviderId = TEXT("uerl.state");
		Result.ParametersJson = TEXT("{\"fields\":[\"SUPPORT\",\"body_velocity\"]}");
		return Result;
	}
	struct FSnapshot
	{
		FUERLInputSampleStamp Stamp;
		TArray<float> Velocity = {1, 2, 3, -4, -5, -6};
		TArray<float> Support = {1, 0};
		int32 Reads = 0;
		bool bAvailable = true;
	};
	FUERLInputSlotBinding Bind(int32 Id, FSnapshot& Snapshot)
	{
		FUERLInputSlotBinding Result; Result.Slot.SlotId = Id;
		for (const FUERLFieldDescriptor& Field : Catalog())
		{
			FUERLInputStateReader Reader; Reader.Field = Field;
			const bool bVelocity = Field.Name == TEXT("body_velocity");
			Reader.Read = [&Snapshot, bVelocity](FUERLInputSampleStamp& Stamp,
				TConstArrayView<float>& Values, FString& Error)
			{
				++Snapshot.Reads;
				if (!Snapshot.bAvailable) { Error = TEXT("host snapshot unavailable"); return false; }
				Stamp = Snapshot.Stamp;
				Values = bVelocity ? MakeArrayView(Snapshot.Velocity) : MakeArrayView(Snapshot.Support);
				return true;
			};
			Result.StateReaders.Add(Field.Name, MoveTemp(Reader));
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLStateInputLifecycleTest, "UERL.Unit.InputProvider.State.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLStateInputLifecycleTest::RunTest(const FString&)
{
	using namespace UERLStateInputTest;
	FSnapshot A, B; B.Support = {0, 1};
	FUERLInputRegistry Registry; FString Error;
	Registry.RegisterFactory(MakeShared<FUERLStateInputFactory>(Catalog()), Error);
	FUERLCompiledInputSet Set;
	const TArray<FUERLInputSpec> Specs = {Spec()};
	TArray<FUERLInputSlotBinding> Bindings = {Bind(2, A), Bind(7, B)};
	if (!TestTrue(TEXT("bind state adapter"), Set.Bind(Registry, Specs, Bindings, Error))) { return false; }
	Bindings.Reset(); // The provider must retain its own callbacks, not the temporary binding array.
	TConstArrayView<float> Values;
	if (!TestTrue(TEXT("bootstrap"), Set.Sample(2, A.Stamp, Values, Error))) { return false; }
	TestTrue(TEXT("literal requested order and values"), TArray<float>(Values) == TArray<float>({1, 0, 1, 2, 3, -4, -5, -6}));
	const TArray<FUERLFieldDescriptor>& Fields = Set.GetFields();
	TestEqual(TEXT("preserve requested first field"), Fields[0].Name, FName(TEXT("support")));
	TestEqual(TEXT("preserve unit"), Fields[1].Unit, FString(TEXT("m/s")));
	TestEqual(TEXT("preserve frame"), Fields[1].CoordinateFrame, FString(TEXT("ground")));
	TestEqual(TEXT("preserve source"), Fields[1].Source, FString(TEXT("robot")));
	TestEqual(TEXT("preserve semantic"), Fields[1].Semantic, FString(TEXT("linear_velocity")));
	TestTrue(TEXT("preserve matrix shape"), Fields[1].Shape == TArray<int32>({2, 3}));
	TestTrue(TEXT("other Slot"), Set.Sample(7, B.Stamp, Values, Error));
	TestEqual(TEXT("other Slot support"), Values[0], 0.0f);
	A.Support[0] = 0;
	TestTrue(TEXT("publication owns values"), Set.Sample(2, A.Stamp, Values, Error));
	TestEqual(TEXT("cached original value"), Values[0], 1.0f);
	TestEqual(TEXT("duplicate read does not recollect"), A.Reads, 2);
	A.Stamp.Sequence = 1; A.Stamp.SolverTimeSeconds = 0.02;
	TestTrue(TEXT("new completed window"), Set.Sample(2, A.Stamp, Values, Error));
	TestEqual(TEXT("new support snapshot"), Values[0], 0.0f);
	const TArray<int32> ResetIds = {2};
	TestTrue(TEXT("sparse reset"), Set.Reset(ResetIds, Error));
	TestEqual(TEXT("host generation untouched by adapter"), A.Stamp.ResetGeneration, int64(0));
	TestTrue(TEXT("other publication preserved"), Set.Sample(7, B.Stamp, Values, Error));
	TestEqual(TEXT("other readers not called again"), B.Reads, 2);
	A.Stamp.ResetGeneration = 1; A.Support[0] = 1;
	TestTrue(TEXT("host reset snapshot accepted"), Set.Sample(2, A.Stamp, Values, Error));
	TestEqual(TEXT("reset value published"), Values[0], 1.0f);
	Set.Release(); Set.Release();
	TestFalse(TEXT("cannot sample after release"), Set.Sample(2, A.Stamp, Values, Error));
	A.Stamp = FUERLInputSampleStamp();
	Bindings = {Bind(2, A)};
	TestTrue(TEXT("rebind"), Set.Bind(Registry, Specs, Bindings, Error));
	TestTrue(TEXT("new lifecycle bootstrap"), Set.Sample(2, A.Stamp, Values, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLStateInputFaultTest, "UERL.Unit.InputProvider.State.SnapshotFaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLStateInputFaultTest::RunTest(const FString&)
{
	using namespace UERLStateInputTest;
	for (int32 Failure = 0; Failure < 6; ++Failure)
	{
		FSnapshot A, B;
		A.Stamp.Sequence = B.Stamp.Sequence = 5;
		A.Stamp.SolverTimeSeconds = B.Stamp.SolverTimeSeconds = 0.10;
		FUERLInputSampleStamp Expected = A.Stamp;
		FUERLInputRegistry Registry; FString Error;
		Registry.RegisterFactory(MakeShared<FUERLStateInputFactory>(Catalog()), Error);
		FUERLCompiledInputSet Set;
		const TArray<FUERLInputSpec> Specs = {Spec()};
		const TArray<FUERLInputSlotBinding> Bindings = {Bind(2, A), Bind(7, B)};
		if (!TestTrue(TEXT("bind"), Set.Bind(Registry, Specs, Bindings, Error))) { return false; }
		switch (Failure)
		{
		case 0: A.Stamp.Sequence = 4; break;
		case 1: A.Stamp.SolverTimeSeconds = 0.08; break;
		case 2: A.Stamp.ResetGeneration = 1; break;
		case 3: A.Support.Pop(); break;
		case 4: A.bAvailable = false; break;
		case 5: A.Support[0] = std::numeric_limits<float>::quiet_NaN(); break;
		}
		TConstArrayView<float> Values;
		TestFalse(FString::Printf(TEXT("fault %d rejects publication"), Failure), Set.Sample(2, Expected, Values, Error));
		TestTrue(TEXT("identify input and Slot"), Error.Contains(TEXT("state")) && Error.Contains(TEXT("Slot 2")));
		TestEqual(TEXT("no partial publication"), Values.Num(), 0);
		A.Stamp = Expected; A.Support = {1, 0}; A.bAvailable = true;
		TestFalse(TEXT("repair needs explicit reset"), Set.Sample(2, Expected, Values, Error));
		TestTrue(TEXT("fault isolated from other Slot"), Set.Sample(7, B.Stamp, Values, Error));
		const TArray<int32> ResetIds = {2};
		TestTrue(TEXT("reset fault"), Set.Reset(ResetIds, Error));
		Expected.ResetGeneration = 1;
		TestFalse(TEXT("pre-reset snapshot rejected"), Set.Sample(2, Expected, Values, Error));
		TestTrue(TEXT("reset again after stale snapshot fault"), Set.Reset(ResetIds, Error));
		Expected.ResetGeneration = 2; A.Stamp = Expected;
		TestTrue(TEXT("host publishes recovered generation"), Set.Sample(2, Expected, Values, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLStateInputBindingTest, "UERL.Unit.InputProvider.State.BindingAndSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLStateInputBindingTest::RunTest(const FString&)
{
	using namespace UERLStateInputTest;
	FUERLStateInputFactory Factory(Catalog()); FString Error;
	FUERLInputSpec Effective;
	TArray<FUERLFieldDescriptor> Fields;
	for (const FString& Json : TArray<FString>{TEXT("{}"), TEXT("{\"fields\":[]}"),
		TEXT("{\"fields\":\"support\"}"), TEXT("{\"fields\":[false]}"),
		TEXT("{\"fields\":[\" support\"]}"), TEXT("{\"fields\":[\"missing\"]}"),
		TEXT("{\"fields\":[\"support\",\"SUPPORT\"]}"), TEXT("{\"fields\":[\"support\"],\"scale\":100}")})
	{
		FUERLInputSpec Invalid = Spec(); Invalid.ParametersJson = Json;
		TestFalse(TEXT("reject invalid selection"), Factory.Validate(Invalid, Effective, Fields, Error));
		TestFalse(TEXT("selection diagnostic"), Error.IsEmpty());
	}
	TestTrue(TEXT("resolve valid selection"), Factory.Validate(Spec(), Effective, Fields, Error));
	TArray<FUERLFieldDescriptor> Restored;
	FUERLInputSpec RestoredSpec;
	TestTrue(TEXT("effective selection round trip"), Factory.Validate(Effective, RestoredSpec, Restored, Error));
	TestEqual(TEXT("canonical first name"), Restored[0].Name.ToString(), FString(TEXT("support")));
	FSnapshot Snapshot;
	FUERLInputRegistry Registry;
	Registry.RegisterFactory(MakeShared<FUERLStateInputFactory>(Catalog()), Error);
	FUERLCompiledInputSet Set;
	const TArray<FUERLInputSpec> Specs = {Spec()};
	TArray<FUERLInputSlotBinding> Bindings = {Bind(2, Snapshot)};
	Bindings[0].StateReaders[TEXT("support")].Field.CoordinateFrame = TEXT("body");
	TestFalse(TEXT("frame mismatch"), Set.Bind(Registry, Specs, Bindings, Error));
	Bindings = {Bind(2, Snapshot)};
	Bindings[0].StateReaders.Remove(TEXT("support"));
	TestFalse(TEXT("missing reader"), Set.Bind(Registry, Specs, Bindings, Error));
	TestEqual(TEXT("failed binding never samples host"), Snapshot.Reads, 0);
	TestEqual(TEXT("failed binding has no output fields"), Set.GetFields().Num(), 0);
	return true;
}
#endif

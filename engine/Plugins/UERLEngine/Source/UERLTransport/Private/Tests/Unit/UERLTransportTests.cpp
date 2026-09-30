#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "UERLBatchLayout.h"
#include "UERLJson.h"
#include "UERLProtocol.h"
#include "UERLTransportAdapter.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLInMemoryTransportTest,
	"UERL.Unit.Transport.InMemoryAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLInMemoryTransportTest::RunTest(const FString& Parameters)
{
	FUERLInMemoryTransport Transport;
	FThreadSafeBool StopRequested{ false };
	FString Error;
	TestTrue(TEXT("in-memory listen"), Transport.Listen(1, Error));
	TestTrue(TEXT("in-memory accept"), Transport.Accept(1, StopRequested, Error));
	Transport.Feed({ 1, 2 });
	Transport.Feed({ 3, 4 });
	uint8 Read[4]{};
	TestTrue(TEXT("exact read joins fed chunks"), Transport.ReadExact(Read, 4, 1, StopRequested));
	TestEqual(TEXT("exact read final byte"), Read[3], static_cast<uint8>(4));
	const uint8 Written[]{ 5, 6, 7 };
	TestTrue(TEXT("exact write succeeds"), Transport.WriteExact(Written, 3, 1, StopRequested));
	const TArray<uint8> ExpectedWritten{ 5, 6, 7 };
	TestEqual(TEXT("written bytes are observable"), Transport.DrainWritten(), ExpectedWritten);
	AddInfo(TEXT("[VERIFY] AC-U4-CPP-003: protocol can use a deterministic byte transport"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLFrameHeaderTest,
	"UERL.Unit.Transport.FrameHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLFrameHeaderTest::RunTest(const FString& Parameters)
{
	UERLProtocol::FFrameHeader Header;
	Header.ProtocolMajor = 2;
	Header.ProtocolMinor = 0;
	Header.Message = UERLProtocol::EMessage::Step;
	Header.Flags = UERLProtocol::Request;
	Header.PayloadLength = 16;
	Header.Sequence = 7;
	Header.Session = FGuid(0x00112233, 0x44554677, 0x8899aabb, 0xccddeeff);
	Header.LayoutId = 0x1020304050607080ull;
	uint8 Bytes[UERLProtocol::FrameHeaderSize];
	Header.Encode(Bytes);

	TestEqual(TEXT("network-order magic byte"), Bytes[0], static_cast<uint8>('U'));
	TestEqual(TEXT("fixed header size"), static_cast<int32>(sizeof(Bytes)), UERLProtocol::FrameHeaderSize);
	TestEqual(TEXT("cross-language header golden"), BytesToHex(Bytes, sizeof(Bytes)),
		TEXT("5545524C000200000101000100000010000000000000000700112233445546778899AABBCCDDEEFF1020304050607080"));
	UERLProtocol::FFrameHeader Decoded;
	FString Error;
	TestTrue(TEXT("decode succeeds"), UERLProtocol::FFrameHeader::Decode(Bytes, Decoded, Error));
	TestEqual(TEXT("sequence round-trips"), Decoded.Sequence, Header.Sequence);
	TestEqual(TEXT("UUID bytes round-trip"), Decoded.Session, Header.Session);
	TestEqual(TEXT("layout id round-trips"), Decoded.LayoutId, Header.LayoutId);
	AddInfo(TEXT("[VERIFY] AC-U4-CPP-001: target frame header is exactly encoded and decoded"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLCanonicalJsonTest,
	"UERL.Unit.Transport.CanonicalJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLCanonicalJsonTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("b"), 1);
	Object->SetStringField(TEXT("a"), TEXT("x"));
	FString Hash;
	FString Error;
	TestTrue(TEXT("canonical hash succeeds"), UERLJson::CanonicalSha256(Object, Hash, Error));
	TestEqual(TEXT("cross-language JCS golden"), Hash,
		TEXT("cdab067e9f3beb32d1252cfd63e492592fecbf591b0d08cadb24bb17f3864246"));
	TSharedPtr<FJsonObject> Parsed;
	const TArray<uint8> NumberJson = UERLJson::ToUtf8(TEXT("{\"numbers\":[1e30,4.50,2e-3,1e-27]}"));
	TestTrue(TEXT("strict number JSON parses"), UERLJson::ParseStrictObject(NumberJson, Parsed, Error));
	FString CanonicalNumbers;
	TestTrue(TEXT("numbers canonicalize"),
		UERLJson::Canonicalize(Parsed->TryGetField(TEXT("numbers")), CanonicalNumbers, Error));
	TestEqual(TEXT("RFC 8785 number formatting"), CanonicalNumbers, TEXT("[1e+30,4.5,0.002,1e-27]"));
	TSharedRef<FJsonObject> UnicodeObject = MakeShared<FJsonObject>();
	FString EmojiKey;
	EmojiKey.AppendChar(0xd83d);
	EmojiKey.AppendChar(0xde00);
	UnicodeObject->SetNumberField(FString::Chr(0xfffd), 1);
	UnicodeObject->SetNumberField(EmojiKey, 2);
	FString CanonicalUnicode;
	TestTrue(TEXT("UTF-16 key order canonicalizes"),
		UERLJson::Canonicalize(MakeShared<FJsonValueObject>(UnicodeObject), CanonicalUnicode, Error));
	TestEqual(TEXT("RFC 8785 UTF-16 property order"), CanonicalUnicode,
		FString(TEXT("{\"")) + EmojiKey + TEXT("\":2,\"") + FString::Chr(0xfffd) + TEXT("\":1}"));
	TSharedPtr<FJsonObject> Rejected;
	TestFalse(TEXT("duplicate key is rejected"),
		UERLJson::ParseStrictObject(UERLJson::ToUtf8(TEXT("{\"a\":1,\"a\":2}")), Rejected, Error));
	TestFalse(TEXT("unsafe JSON integer is rejected before double rounding"),
		UERLJson::ParseStrictObject(UERLJson::ToUtf8(TEXT("{\"value\":9007199254740992}")), Rejected, Error));
	const TArray<uint8> InvalidUtf8{ '{', '"', 'a', '"', ':', '"', 0xc0, 0xaf, '"', '}' };
	TestFalse(TEXT("invalid UTF-8 is rejected"), UERLJson::ParseStrictObject(InvalidUtf8, Rejected, Error));
	AddInfo(TEXT("[VERIFY] AC-U4-CPP-002: canonical JSON hash matches Python golden"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLBatchPaddingTest,
	"UERL.Unit.Transport.BatchPadding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLBatchPaddingTest::RunTest(const FString& Parameters)
{
	auto Field = [](const TCHAR* Name)
	{
		FUERLFieldDescriptor Result;
		Result.Name = Name;
		Result.Unit = TEXT("unit");
		Result.CoordinateFrame = TEXT("test-frame");
		Result.Semantic = TEXT("test-value");
		Result.Source = TEXT("uerl.test.v1");
		return Result;
	};
	FUERLBatchSchema Schema;
	Schema.ActionFields = { { Field(TEXT("robot.action.a")), 0 }, { Field(TEXT("robot.action.b")), 1 } };
	Schema.StateFields = { { Field(TEXT("robot.state")), 0 } };
	Schema.ActionWidth = 2;
	Schema.StateWidth = 1;
	UERLBatchLayout::FSet Layouts;
	FString Error;
	TestTrue(TEXT("test layout compiles"), UERLBatchLayout::Compile(3, Schema, Layouts, Error));
	const UERLBatchLayout::FSegment* TerrainSegment = Layouts.ResetRequest.Find(TEXT("terrain_level"));
	TestNotNull(TEXT("reset layout contains terrain levels"), TerrainSegment);
	if (TerrainSegment)
	{
		TestEqual(TEXT("terrain levels use uint16"), TerrainSegment->DType, FString(TEXT("uint16")));
		TestTrue(TEXT("terrain levels cover every Slot"), TerrainSegment->Shape == TArray<int32>({ 3 }));
		TestEqual(TEXT("terrain levels are aligned"), TerrainSegment->Offset, 8u);
	}
	TArray<uint8> ResetPayload;
	ResetPayload.SetNumZeroed(Layouts.ResetRequest.PayloadLength);
	if (TerrainSegment)
	{
		ResetPayload[TerrainSegment->Offset] = 2;
		ResetPayload[TerrainSegment->Offset + 2] = 0x34;
		ResetPayload[TerrainSegment->Offset + 3] = 0x12;
		ResetPayload[TerrainSegment->Offset + 4] = 7;
	}
	TArray<uint16> TerrainLevels;
	TestTrue(TEXT("terrain levels decode little endian"), UERLBatchLayout::DecodeTerrainLevels(
		Layouts.ResetRequest, ResetPayload, 3, TerrainLevels, Error));
	TestTrue(TEXT("decoded terrain levels"), TerrainLevels == TArray<uint16>({ 2, 0x1234, 7 }));
	TArray<uint8> Payload;
	Payload.SetNumZeroed(Layouts.StepAction.PayloadLength);
	float ActionsData[6]{};
	TestTrue(TEXT("zero padding is accepted"), UERLBatchLayout::DecodeActions(
		Layouts.StepAction, Payload, { ActionsData, 3, 2 }, Error));
	Payload[12] = 1;
	TestFalse(TEXT("non-zero alignment padding is rejected"), UERLBatchLayout::DecodeActions(
		Layouts.StepAction, Payload, { ActionsData, 3, 2 }, Error));
	AddInfo(TEXT("[VERIFY] AC-U4-CPP-004: binary batch padding is canonical zero"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLObservationSerializationTest,
	"UERL.Unit.Transport.ObservationSerialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLObservationSerializationTest::RunTest(const FString& Parameters)
{
	FUERLFieldDescriptor Field;
	Field.Name = TEXT("robot.body.2.body_linear_velocity");
	Field.Shape = { 3 };
	Field.Unit = TEXT("m/s");
	Field.CoordinateFrame = TEXT("slot/world");
	Field.Semantic = TEXT("body_linear_velocity");
	Field.Source = TEXT("uerl.robot");
	Field.Observation.Type = EUERLObservationType::BodyLinearVelocity;
	Field.Observation.BodyName = TEXT("pole");
	Field.Observation.BodyIndex = 2;
	Field.Width = 3;
	const TSharedRef<FJsonValue> Value = UERLBatchLayout::FieldArray({ { Field, 0 } });
	const TArray<TSharedPtr<FJsonValue>>& Fields = Value->AsArray();
	TestEqual(TEXT("one field is serialized"), Fields.Num(), 1);
	if (Fields.Num() == 1)
	{
		const TSharedPtr<FJsonObject> Object = Fields[0]->AsObject();
		const TSharedPtr<FJsonObject>* Extensions = nullptr;
		TestTrue(TEXT("observation extensions are serialized"), Object->TryGetObjectField(TEXT("extensions"), Extensions));
		if (Extensions && Extensions->IsValid())
		{
			double BodyIndex = INDEX_NONE;
			FString BodyName, ObservationType;
			TestTrue(TEXT("body index is preserved"), (*Extensions)->TryGetNumberField(TEXT("body_index"), BodyIndex));
			TestTrue(TEXT("body name is preserved"), (*Extensions)->TryGetStringField(TEXT("body_name"), BodyName));
			TestTrue(TEXT("observation type is preserved"), (*Extensions)->TryGetStringField(TEXT("observation_type"), ObservationType));
			TestTrue(TEXT("serialized body index"), FMath::IsNearlyEqual(BodyIndex, 2.0));
			TestEqual(TEXT("serialized body name"), BodyName, FString(TEXT("pole")));
			TestEqual(TEXT("serialized observation type"), ObservationType, FString(TEXT("body_linear_velocity")));
		}
	}
	AddInfo(TEXT("[VERIFY] AC-U4-CPP-005: observation binding metadata survives selected schema serialization"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLStepDecimationLayoutTest,
	"UERL.Unit.Transport.AC_PY_UNIT_DT_009.StepDecimationLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLStepDecimationLayoutTest::RunTest(const FString& Parameters)
{
	FUERLFieldDescriptor ActionField;
	ActionField.Name = TEXT("robot.action.target");
	ActionField.Shape = {};
	ActionField.Unit = TEXT("unit");
	ActionField.CoordinateFrame = TEXT("test-frame");
	ActionField.Semantic = TEXT("target");
	ActionField.Source = TEXT("uerl.test");
	ActionField.Width = 1;
	FUERLBatchSchema Schema;
	Schema.ActionFields = { { ActionField, 0 } };
	Schema.StateFields = { { ActionField, 0 } };
	Schema.ActionWidth = 1;
	Schema.StateWidth = 1;

	UERLBatchLayout::FSet Layouts;
	FString Error;
	TestTrue(TEXT("layout compiles with step decimation"), UERLBatchLayout::Compile(2, Schema, Layouts, Error));
	const UERLBatchLayout::FSegment* Segment = Layouts.StepAction.Find(TEXT("step_decimation"));
	TestNotNull(TEXT("step_decimation segment is present"), Segment);
	if (Segment == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("step_decimation dtype is int32"), Segment->DType, FString(TEXT("int32")));
	TestTrue(TEXT("step_decimation shape is one scalar"), Segment->Shape == TArray<int32>({ 1 }));
	TestEqual(TEXT("step_decimation is one int32"), Segment->ByteLength, 4u);

	TArray<uint8> Payload;
	Payload.SetNumZeroed(Layouts.StepAction.PayloadLength);
	Payload[Segment->Offset] = 7;
	int32 Decimation = 0;
	TestTrue(TEXT("valid little-endian scalar decodes"), UERLBatchLayout::DecodeStepDecimation(
		Layouts.StepAction, Payload, Decimation, Error));
	TestEqual(TEXT("decoded scalar is seven"), Decimation, 7);

	TArray<uint8> NegativePayload = Payload;
	NegativePayload[Segment->Offset] = 0xff;
	NegativePayload[Segment->Offset + 1] = 0xff;
	NegativePayload[Segment->Offset + 2] = 0xff;
	NegativePayload[Segment->Offset + 3] = 0xff;
	TestFalse(TEXT("negative scalar is rejected"), UERLBatchLayout::DecodeStepDecimation(
		Layouts.StepAction, NegativePayload, Decimation, Error));

	TArray<uint8> ShortPayload = Payload;
	ShortPayload.RemoveAt(ShortPayload.Num() - 1);
	TestFalse(TEXT("wrong payload length is rejected"), UERLBatchLayout::DecodeStepDecimation(
		Layouts.StepAction, ShortPayload, Decimation, Error));

	UERLBatchLayout::FLayout WrongDType = Layouts.StepAction;
	WrongDType.Segments.Last().DType = TEXT("float32");
	TestFalse(TEXT("wrong scalar dtype is rejected"), UERLBatchLayout::DecodeStepDecimation(
		WrongDType, Payload, Decimation, Error));

	AddInfo(TEXT("[VERIFY] AC_PY_UNIT_DT_009: UE decodes one little-endian int32 step_decimation scalar and rejects malformed segments"));
	return true;
}

#endif

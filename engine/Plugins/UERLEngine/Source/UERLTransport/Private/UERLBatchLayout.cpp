#include "UERLBatchLayout.h"

#include "UERLJson.h"
#include "UERLProtocol.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using namespace UERLBatchLayout;

	const TCHAR* KindName(EKind Kind)
	{
		switch (Kind)
		{
		case EKind::InitialState: return TEXT("initial_state");
		case EKind::StepAction: return TEXT("step_action");
		case EKind::StepResult: return TEXT("step_result");
		case EKind::ResetRequest: return TEXT("reset_request");
		case EKind::ResetResult: return TEXT("reset_result");
		default: return TEXT("unknown");
		}
	}

	// Every segment starts on an 8-byte boundary so the field-major payload
	// has one deterministic layout on both the UE and Python sides.
	uint32 Align8(uint32 Value) { return (Value + 7u) & ~7u; }

	uint32 DTypeSize(const FString& DType)
	{
		if (DType == TEXT("float32")) { return 4; }
		if (DType == TEXT("int32")) { return 4; }
		if (DType == TEXT("uint64")) { return 8; }
		if (DType == TEXT("uint16")) { return 2; }
		return 1;
	}

	int32 ShapeWidth(const TArray<int32>& Shape)
	{
		int32 Width = 1;
		for (int32 Dimension : Shape) { Width *= Dimension; }
		return Width;
	}

	void AddSegment(FLayout& Layout, const FString& Name, const FString& DType,
		const TArray<int32>& Shape, int32 BatchColumn = INDEX_NONE)
	{
		FSegment& Segment = Layout.Segments.AddDefaulted_GetRef();
		Segment.Name = Name;
		Segment.DType = DType;
		Segment.Shape = Shape;
		Segment.Offset = Align8(Layout.PayloadLength);
		Segment.ByteLength = DTypeSize(DType) * ShapeWidth(Shape);
		Segment.BatchColumn = BatchColumn;
		Layout.PayloadLength = Segment.Offset + Segment.ByteLength;
	}

	TSharedRef<FJsonObject> CoreJson(const FLayout& Layout)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("layout_kind"), KindName(Layout.Kind));
		Root->SetStringField(TEXT("byte_order"), TEXT("little"));
		Root->SetNumberField(TEXT("alignment"), 8);
		Root->SetNumberField(TEXT("payload_length"), Layout.PayloadLength);
		TArray<TSharedPtr<FJsonValue>> Segments;
		for (const FSegment& Segment : Layout.Segments)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Segment.Name);
			Item->SetStringField(TEXT("dtype"), Segment.DType);
			TArray<TSharedPtr<FJsonValue>> Shape;
			for (int32 Dimension : Segment.Shape) { Shape.Add(MakeShared<FJsonValueNumber>(Dimension)); }
			Item->SetArrayField(TEXT("shape"), Shape);
			Item->SetNumberField(TEXT("offset"), Segment.Offset);
			Item->SetNumberField(TEXT("byte_length"), Segment.ByteLength);
			Segments.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("segments"), Segments);
		return Root;
	}

	bool FinishLayout(FLayout& Layout, FString& OutError)
	{
		// Hash the negotiated metadata, not the derived offsets alone, so a
		// change to protocol version, byte order, alignment, or field order
		// creates a new layout identity.
		TSharedRef<FJsonObject> HashInput = MakeShared<FJsonObject>();
		HashInput->SetObjectField(TEXT("protocol_version"), []
		{
			TSharedRef<FJsonObject> Version = MakeShared<FJsonObject>();
			Version->SetNumberField(TEXT("major"), UERLProtocol::Major);
			Version->SetNumberField(TEXT("minor"), 0);
			return Version;
		}());
		HashInput->SetStringField(TEXT("layout_kind"), KindName(Layout.Kind));
		HashInput->SetNumberField(TEXT("batch_size"), Layout.BatchSize);
		HashInput->SetStringField(TEXT("byte_order"), TEXT("little"));
		HashInput->SetNumberField(TEXT("alignment"), 8);
		HashInput->SetArrayField(TEXT("ordered_segments"), CoreJson(Layout)->GetArrayField(TEXT("segments")));
		if (!UERLJson::CanonicalSha256(HashInput, Layout.LayoutHash, OutError)) { return false; }
		if (Layout.LayoutHash.Len() != 64) { OutError = TEXT("invalid layout hash length"); return false; }
		if (Layout.PayloadLength > UERLProtocol::MaxDataPayloadBytes)
		{
			OutError = TEXT("layout exceeds the data payload limit");
			return false;
		}
		Layout.LayoutId = FCString::Strtoui64(*Layout.LayoutHash.Left(16), nullptr, 16);
		return true;
	}

	void AddFields(FLayout& Layout, int32 NumSlots, const TArray<FUERLBatchFieldBinding>& Bindings)
	{
		for (const FUERLBatchFieldBinding& Binding : Bindings)
		{
			TArray<int32> WireShape{ NumSlots };
			WireShape.Append(Binding.Field.Shape);
			AddSegment(Layout, Binding.Field.Name.ToString(), Binding.Field.DType, WireShape, Binding.Column);
		}
	}

	void Put16LE(uint8* Out, uint16 Value) { Out[0] = static_cast<uint8>(Value); Out[1] = static_cast<uint8>(Value >> 8); }
	void Put32LE(uint8* Out, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index) { Out[Index] = static_cast<uint8>(Value >> (8 * Index)); }
	}
	void Put64LE(uint8* Out, uint64 Value)
	{
		for (int32 Index = 0; Index < 8; ++Index) { Out[Index] = static_cast<uint8>(Value >> (8 * Index)); }
	}
	uint16 Get16LE(const uint8* In)
	{
		return static_cast<uint16>(In[0]) | (static_cast<uint16>(In[1]) << 8);
	}
	uint32 Get32LE(const uint8* In)
	{
		return static_cast<uint32>(In[0]) | (static_cast<uint32>(In[1]) << 8)
			| (static_cast<uint32>(In[2]) << 16) | (static_cast<uint32>(In[3]) << 24);
	}

	bool HasZeroPadding(const FLayout& Layout, const TArray<uint8>& Payload, FString& OutError)
	{
		// Zero padding and unused Reset State rows prevent uninitialized memory
		// from becoming part of a deterministic wire payload.
		uint32 Cursor = 0;
		for (const FSegment& Segment : Layout.Segments)
		{
			if (Segment.Offset < Cursor || Segment.Offset + Segment.ByteLength > Layout.PayloadLength)
			{
				OutError = TEXT("layout segments overlap or exceed payload length");
				return false;
			}
			for (uint32 Index = Cursor; Index < Segment.Offset; ++Index)
			{
				if (Payload[Index] != 0) { OutError = TEXT("binary batch padding must be zero"); return false; }
			}
			Cursor = Segment.Offset + Segment.ByteLength;
		}
		for (uint32 Index = Cursor; Index < Layout.PayloadLength; ++Index)
		{
			if (Payload[Index] != 0) { OutError = TEXT("binary batch padding must be zero"); return false; }
		}
		return true;
	}
}

const UERLBatchLayout::FSegment* UERLBatchLayout::FLayout::Find(const FString& Name) const
{
	return Segments.FindByPredicate([&Name](const FSegment& Segment) { return Segment.Name == Name; });
}

TSharedRef<FJsonObject> UERLBatchLayout::FLayout::ToJson() const
{
	TSharedRef<FJsonObject> Root = CoreJson(*this);
	Root->SetStringField(TEXT("layout_id"), FString::Printf(TEXT("%016llx"), LayoutId));
	Root->SetStringField(TEXT("layout_hash"), LayoutHash);
	return Root;
}

const UERLBatchLayout::FLayout* UERLBatchLayout::FSet::Find(EKind Kind) const
{
	switch (Kind)
	{
	case EKind::InitialState: return &InitialState;
	case EKind::StepAction: return &StepAction;
	case EKind::StepResult: return &StepResult;
	case EKind::ResetRequest: return &ResetRequest;
	case EKind::ResetResult: return &ResetResult;
	default: return nullptr;
	}
}

bool UERLBatchLayout::Compile(int32 NumSlots, const FUERLBatchSchema& Schema, FSet& OutLayouts, FString& OutError)
{
	if (NumSlots <= 0 || !Schema.IsValid()) { OutError = TEXT("cannot compile layouts for invalid schema"); return false; }
	OutLayouts = FSet();
	OutLayouts.InitialState.Kind = EKind::InitialState;
	OutLayouts.InitialState.BatchSize = NumSlots;
	AddFields(OutLayouts.InitialState, NumSlots, Schema.StateFields);
	AddSegment(OutLayouts.InitialState, TEXT("system.episode_index"), TEXT("uint64"), { NumSlots });
	OutLayouts.StepAction.Kind = EKind::StepAction;
	OutLayouts.StepAction.BatchSize = NumSlots;
	AddFields(OutLayouts.StepAction, NumSlots, Schema.ActionFields);
	AddSegment(OutLayouts.StepAction, TEXT("step_decimation"), TEXT("int32"), { 1 });
	OutLayouts.StepResult.Kind = EKind::StepResult;
	OutLayouts.StepResult.BatchSize = NumSlots;
	AddFields(OutLayouts.StepResult, NumSlots, Schema.StateFields);
	// System segments are appended after user fields so Task field order stays
	// stable while the protocol can add validity and fault metadata uniformly.
	AddSegment(OutLayouts.StepResult, TEXT("system.state_valid"), TEXT("uint8"), { NumSlots });
	AddSegment(OutLayouts.StepResult, TEXT("system.slot_fault_code"), TEXT("uint16"), { NumSlots });
	OutLayouts.ResetRequest.Kind = EKind::ResetRequest;
	OutLayouts.ResetRequest.BatchSize = NumSlots;
	AddSegment(OutLayouts.ResetRequest, TEXT("system.reset_mask"), TEXT("uint8"), { (NumSlots + 7) / 8 });
	AddSegment(OutLayouts.ResetRequest, TEXT("terrain_level"), TEXT("uint16"), { NumSlots });
	if (Schema.ResetWidth > 0)
	{
		AddSegment(OutLayouts.ResetRequest, TEXT("robot.reset.values"), TEXT("float32"), { NumSlots, Schema.ResetWidth });
	}
	OutLayouts.ResetResult.Kind = EKind::ResetResult;
	OutLayouts.ResetResult.BatchSize = NumSlots;
	AddFields(OutLayouts.ResetResult, NumSlots, Schema.StateFields);
	AddSegment(OutLayouts.ResetResult, TEXT("system.reset_mask"), TEXT("uint8"), { (NumSlots + 7) / 8 });
	AddSegment(OutLayouts.ResetResult, TEXT("system.episode_index"), TEXT("uint64"), { NumSlots });
	return FinishLayout(OutLayouts.InitialState, OutError)
		&& FinishLayout(OutLayouts.StepAction, OutError)
		&& FinishLayout(OutLayouts.StepResult, OutError)
		&& FinishLayout(OutLayouts.ResetRequest, OutError)
		&& FinishLayout(OutLayouts.ResetResult, OutError);
}

TSharedRef<FJsonObject> ObservationExtensionsJson(const FUERLObservationBinding& Observation)
{
	TSharedRef<FJsonObject> Extensions = MakeShared<FJsonObject>();
	Extensions->SetNumberField(TEXT("body_index"), Observation.BodyIndex);
	Extensions->SetStringField(TEXT("body_name"), Observation.BodyName.ToString());
	Extensions->SetStringField(TEXT("observation_type"), UERLObservationTypeName(Observation.Type));
	if (Observation.Type == EUERLObservationType::JointPosition
		|| Observation.Type == EUERLObservationType::JointVelocity)
	{
		Extensions->SetNumberField(TEXT("joint_index"), Observation.JointIndex);
		Extensions->SetStringField(TEXT("joint_name"), Observation.JointName.ToString());
	}
	return Extensions;
}

TSharedRef<FJsonObject> ActuatorExtensionsJson(const TArray<FUERLActuatorColumnDescriptor>& Columns)
{
	TSharedRef<FJsonObject> Extensions = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FUERLActuatorColumnDescriptor& Column : Columns)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Column.Index);
		Item->SetStringField(TEXT("joint"), Column.JointName.ToString());
		Item->SetNumberField(TEXT("joint_index"), Column.JointIndex);
		Item->SetStringField(TEXT("coordinate_type"), Column.CoordinateType);
		Item->SetStringField(TEXT("unit"), Column.Unit);
		Item->SetStringField(TEXT("target_mode"), Column.TargetMode);
		Values.Add(MakeShared<FJsonValueObject>(Item));
	}
	Extensions->SetArrayField(TEXT("columns"), Values);
	return Extensions;
}

TSharedRef<FJsonValue> UERLBatchLayout::FieldArray(const TArray<FUERLBatchFieldBinding>& Fields)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (const FUERLBatchFieldBinding& Binding : Fields)
	{
		const FUERLFieldDescriptor& Field = Binding.Field;
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Field.Name.ToString());
		Item->SetStringField(TEXT("dtype"), Field.DType);
		TArray<TSharedPtr<FJsonValue>> Shape;
		for (int32 Dimension : Field.Shape) { Shape.Add(MakeShared<FJsonValueNumber>(Dimension)); }
		Item->SetArrayField(TEXT("shape"), Shape);
		Item->SetStringField(TEXT("unit"), Field.Unit);
		Item->SetStringField(TEXT("frame"), Field.CoordinateFrame);
		Item->SetStringField(TEXT("semantic"), Field.Semantic);
		Item->SetStringField(TEXT("source"), Field.Source);
		if (Field.Observation.Type != EUERLObservationType::None)
		{
			Item->SetObjectField(TEXT("extensions"), ObservationExtensionsJson(Field.Observation));
		}
		else if (!Field.ActuatorColumns.IsEmpty())
		{
			Item->SetObjectField(TEXT("extensions"), ActuatorExtensionsJson(Field.ActuatorColumns));
		}
		Result.Add(MakeShared<FJsonValueObject>(Item));
	}
	return MakeShared<FJsonValueArray>(Result);
}

	bool UERLBatchLayout::DecodeActions(
	const FLayout& Layout, const TArray<uint8>& Payload, FUERLMutableBatchView Actions, FString& OutError)
{
	if (Payload.Num() != static_cast<int32>(Layout.PayloadLength) || !Actions.IsValid())
	{
		OutError = TEXT("Step payload length or Action staging view is invalid");
		return false;
	}
	if (!HasZeroPadding(Layout, Payload, OutError)) { return false; }
	for (const FSegment& Segment : Layout.Segments)
	{
		if (Segment.DType != TEXT("float32") || Segment.BatchColumn < 0) { continue; }
		const int32 FieldWidth = Segment.ByteLength / (Actions.NumRows * 4);
		for (int32 Row = 0; Row < Actions.NumRows; ++Row)
		{
			for (int32 Element = 0; Element < FieldWidth; ++Element)
			{
				const uint32 Bits = Get32LE(Payload.GetData() + Segment.Offset + (Row * FieldWidth + Element) * 4);
				float Value = 0.0f;
				FMemory::Memcpy(&Value, &Bits, sizeof(float));
				if (!FMath::IsFinite(Value)) { OutError = TEXT("Step contains non-finite Action data"); return false; }
				Actions.At(Row, Segment.BatchColumn + Element) = Value;
			}
		}
	}
	return true;
}

bool UERLBatchLayout::DecodeStepDecimation(
	const FLayout& Layout, const TArray<uint8>& Payload, int32& OutDecimation, FString& OutError)
{
	const FSegment* Segment = Layout.Find(TEXT("step_decimation"));
	if (!Segment || Segment->DType != TEXT("int32") || Segment->Shape != TArray<int32>({ 1 })
		|| Payload.Num() != static_cast<int32>(Layout.PayloadLength))
	{
		OutError = TEXT("Step decimation segment or payload is invalid");
		return false;
	}
	if (!HasZeroPadding(Layout, Payload, OutError)) { return false; }
	const uint32 Bits = Get32LE(Payload.GetData() + Segment->Offset);
	OutDecimation = static_cast<int32>(Bits);
	if (OutDecimation <= 0)
	{
		OutError = TEXT("Step decimation must be positive");
		return false;
	}
	return true;
}

bool UERLBatchLayout::DecodeResetMask(
	const FLayout& Layout, const TArray<uint8>& Payload, int32 NumSlots, TArray<int32>& OutSlots, FString& OutError)
{
	const FSegment* Mask = Layout.Find(TEXT("system.reset_mask"));
	if (!Mask || Payload.Num() != static_cast<int32>(Layout.PayloadLength))
	{
		OutError = TEXT("Reset payload length or layout is invalid");
		return false;
	}
	if (!HasZeroPadding(Layout, Payload, OutError)) { return false; }
	// The final byte is only partially used when the Slot count is not a
	// multiple of eight; accepting extra bits would acknowledge nonexistent Slots.
	const int32 UsedBits = NumSlots % 8;
	if (UsedBits != 0 && (Payload[Mask->Offset + Mask->ByteLength - 1] & ~((1u << UsedBits) - 1u)) != 0)
	{
		OutError = TEXT("Reset mask has non-zero unused bits");
		return false;
	}
	OutSlots.Reset();
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		if ((Payload[Mask->Offset + Slot / 8] & (1u << (Slot % 8))) != 0) { OutSlots.Add(Slot); }
	}
	return true;
}

bool UERLBatchLayout::DecodeTerrainLevels(
	const FLayout& Layout, const TArray<uint8>& Payload, int32 NumSlots,
	TArray<uint16>& OutTerrainLevels, FString& OutError)
{
	const FSegment* Terrain = Layout.Find(TEXT("terrain_level"));
	if (!Terrain || Terrain->DType != TEXT("uint16") || Terrain->Shape != TArray<int32>({ NumSlots })
		|| Payload.Num() != static_cast<int32>(Layout.PayloadLength))
	{
		OutError = TEXT("Reset terrain-level segment or payload is invalid");
		return false;
	}
	if (!HasZeroPadding(Layout, Payload, OutError)) { return false; }
	OutTerrainLevels.SetNum(NumSlots);
	for (int32 Slot = 0; Slot < NumSlots; ++Slot)
	{
		OutTerrainLevels[Slot] = Get16LE(Payload.GetData() + Terrain->Offset + Slot * sizeof(uint16));
	}
	return true;
}

bool UERLBatchLayout::DecodeResetValues(
	const FLayout& Layout, const TArray<uint8>& Payload, int32 NumSlots,
	TArray<float>& OutValues, FString& OutError)
{
	const FSegment* Values = Layout.Find(TEXT("robot.reset.values"));
	if (!Values)
	{
		OutValues.Reset();
		return true;
	}
	if (Values->DType != TEXT("float32") || Values->Shape.Num() != 2 || Values->Shape[0] != NumSlots
		|| Values->Shape[1] <= 0 || Payload.Num() != static_cast<int32>(Layout.PayloadLength))
	{
		OutError = TEXT("Robot reset values segment or payload is invalid");
		return false;
	}
	if (!HasZeroPadding(Layout, Payload, OutError)) { return false; }
	const int32 Width = Values->Shape[1];
	OutValues.SetNumUninitialized(NumSlots * Width);
	for (int32 Index = 0; Index < OutValues.Num(); ++Index)
	{
		uint32 Bits = Get32LE(Payload.GetData() + Values->Offset + Index * sizeof(float));
		FMemory::Memcpy(&OutValues[Index], &Bits, sizeof(float));
		if (!FMath::IsFinite(OutValues[Index]))
		{
			OutError = TEXT("Robot reset values must be finite");
			OutValues.Reset();
			return false;
		}
	}
	return true;
}

bool UERLBatchLayout::EncodeState(
	const FLayout& Layout,
	FUERLMutableBatchView States,
	FUERLFaultBatchView Faults,
	const TArray<uint64>& Episodes,
	const TArray<int32>* ResetSlots,
	TArray<uint8>& OutPayload,
	FString& OutError)
{
	if (!States.IsValid() || !Faults.IsValid() || Episodes.Num() != States.NumRows || Faults.NumRows != States.NumRows)
	{
		OutError = TEXT("State staging views do not match the negotiated Slot count");
		return false;
	}
	TBitArray<> Included(true, States.NumRows);
	if (ResetSlots)
	{
		Included.Init(false, States.NumRows);
		for (int32 Slot : *ResetSlots) { if (Included.IsValidIndex(Slot)) { Included[Slot] = true; } }
	}
	OutPayload.SetNumZeroed(Layout.PayloadLength);
	// Start from zero so alignment gaps and non-selected Reset State rows are
	// deterministic and cannot leak stale staging values.
	for (const FSegment& Segment : Layout.Segments)
	{
		if (Segment.DType == TEXT("float32") && Segment.BatchColumn >= 0)
		{
			const int32 FieldWidth = Segment.ByteLength / (States.NumRows * 4);
			for (int32 Row = 0; Row < States.NumRows; ++Row)
			{
				for (int32 Element = 0; Element < FieldWidth; ++Element)
				{
					const float Value = Included[Row] ? States.At(Row, Segment.BatchColumn + Element) : 0.0f;
					if (!FMath::IsFinite(Value)) { OutError = TEXT("non-finite State reached BatchCodec"); return false; }
					uint32 Bits = 0;
					FMemory::Memcpy(&Bits, &Value, sizeof(float));
					Put32LE(OutPayload.GetData() + Segment.Offset + (Row * FieldWidth + Element) * 4, Bits);
				}
			}
		}
		else if (Segment.Name == TEXT("system.state_valid"))
		{
			for (int32 Row = 0; Row < States.NumRows; ++Row) { OutPayload[Segment.Offset + Row] = Included[Row] && Faults.At(Row) == 0 ? 1 : 0; }
		}
		else if (Segment.Name == TEXT("system.slot_fault_code"))
		{
			for (int32 Row = 0; Row < States.NumRows; ++Row) { Put16LE(OutPayload.GetData() + Segment.Offset + Row * 2, Included[Row] ? Faults.At(Row) : 0); }
		}
		else if (Segment.Name == TEXT("system.reset_mask") && ResetSlots)
		{
			for (int32 Slot : *ResetSlots) { OutPayload[Segment.Offset + Slot / 8] |= static_cast<uint8>(1u << (Slot % 8)); }
		}
		else if (Segment.Name == TEXT("system.episode_index"))
		{
			for (int32 Row = 0; Row < States.NumRows; ++Row) { Put64LE(OutPayload.GetData() + Segment.Offset + Row * 8, Episodes[Row]); }
		}
	}
	return true;
}

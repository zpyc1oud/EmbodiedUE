#include "UERLPolicyArtifact.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr int32 GUERLPolicyArtifactMagicSize = 8;
	constexpr ANSICHAR GUERLPolicyArtifactMagic[GUERLPolicyArtifactMagicSize] = {
		'U', 'E', 'R', 'L', 'P', 'O', 'L', '2'};
	constexpr uint32 GUERLPolicyArtifactFormatVersion = 1;

	bool ArtifactFail(FString& OutError, const FString& Message)
	{
		OutError = Message;
		return false;
	}

	class FArtifactReader
	{
	public:
		explicit FArtifactReader(TConstArrayView<uint8> InBytes)
			: Bytes(InBytes)
		{
		}

		template <typename T>
		bool Read(T& Out)
		{
			if (Offset > static_cast<SIZE_T>(Bytes.Num())
				|| sizeof(T) > static_cast<SIZE_T>(Bytes.Num()) - Offset)
			{
				return false;
			}
			FMemory::Memcpy(&Out, Bytes.GetData() + Offset, sizeof(T));
			Offset += sizeof(T);
			return true;
		}

		bool ReadBytes(uint64 Count, TArray<uint8>& Out)
		{
			if (Count > static_cast<uint64>(TNumericLimits<int32>::Max()))
			{
				return false;
			}
			const int32 ByteCount = static_cast<int32>(Count);
			if (Offset > static_cast<SIZE_T>(Bytes.Num())
				|| static_cast<SIZE_T>(ByteCount) > static_cast<SIZE_T>(Bytes.Num()) - Offset)
			{
				return false;
			}
			Out.SetNumUninitialized(ByteCount);
			FMemory::Memcpy(Out.GetData(), Bytes.GetData() + Offset, ByteCount);
			Offset += ByteCount;
			return true;
		}

		bool AtEnd() const { return Offset == static_cast<SIZE_T>(Bytes.Num()); }

	private:
		TConstArrayView<uint8> Bytes;
		SIZE_T Offset = 0;
	};
}

void FUERLPolicyArtifact::Reset()
{
	bLoaded = false;
	Version = 0;
	Task = NAME_None;
	Robot = NAME_None;
	Observation.Reset();
	Action.Reset();
	Runtime.Reset();
	ArtifactTiming = FUERLPolicyArtifactTiming();
	Onnx.Reset();
}

bool FUERLPolicyArtifact::Load(const FString& Path, FString& OutError)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		return ArtifactFail(OutError, FString::Printf(TEXT("could not load policy artifact '%s'"), *Path));
	}
	return LoadFromBytes(Bytes, OutError);
}

bool FUERLPolicyArtifact::LoadFromBytes(TConstArrayView<uint8> Bytes, FString& OutError)
{
	Reset();
	OutError.Reset();

	FArtifactReader Reader(Bytes);
	ANSICHAR Magic[GUERLPolicyArtifactMagicSize] = {};
	for (int32 Index = 0; Index < GUERLPolicyArtifactMagicSize; ++Index)
	{
		if (!Reader.Read(Magic[Index]))
		{
			return ArtifactFail(OutError, TEXT("artifact header is truncated"));
		}
	}
	if (FMemory::Memcmp(Magic, GUERLPolicyArtifactMagic, GUERLPolicyArtifactMagicSize) != 0)
	{
		return ArtifactFail(OutError, TEXT("artifact magic does not match UERLPOL2"));
	}

	uint32 FormatVersion = 0;
	uint32 JsonLength = 0;
	if (!Reader.Read(FormatVersion) || !Reader.Read(JsonLength))
	{
		return ArtifactFail(OutError, TEXT("artifact header is truncated"));
	}
	if (FormatVersion != GUERLPolicyArtifactFormatVersion)
	{
		return ArtifactFail(
			OutError,
			FString::Printf(
				TEXT("unsupported format_version %u; expected %u"),
				FormatVersion,
				GUERLPolicyArtifactFormatVersion));
	}

	TArray<uint8> JsonBytes;
	if (!Reader.ReadBytes(JsonLength, JsonBytes))
	{
		return ArtifactFail(OutError, TEXT("JSON segment is truncated"));
	}

	uint64 OnnxLength = 0;
	if (!Reader.Read(OnnxLength))
	{
		return ArtifactFail(OutError, TEXT("onnx_length field is truncated"));
	}
	TArray<uint8> OnnxBytes;
	if (!Reader.ReadBytes(OnnxLength, OnnxBytes))
	{
		return ArtifactFail(OutError, TEXT("onnx_length exceeds remaining bytes"));
	}
	if (!Reader.AtEnd())
	{
		return ArtifactFail(OutError, TEXT("artifact contains trailing data after ONNX segment"));
	}

	TArray<char> JsonNullTerminated;
	JsonNullTerminated.Append(reinterpret_cast<const char*>(JsonBytes.GetData()), JsonBytes.Num());
	JsonNullTerminated.Add('\0');
	const FString JsonText = UTF8_TO_TCHAR(JsonNullTerminated.GetData());
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(JsonReader, Root) || !Root.IsValid())
	{
		return ArtifactFail(OutError, TEXT("JSON segment is truncated or invalid"));
	}

	FString TaskText;
	FString RobotText;
	if (!Root->TryGetStringField(TEXT("task_id"), TaskText) || TaskText.IsEmpty())
	{
		return ArtifactFail(OutError, TEXT("missing required field 'task_id'"));
	}
	if (!Root->TryGetStringField(TEXT("robot_id"), RobotText) || RobotText.IsEmpty())
	{
		return ArtifactFail(OutError, TEXT("missing required field 'robot_id'"));
	}

	const TSharedPtr<FJsonObject>* ObservationObject = nullptr;
	const TSharedPtr<FJsonObject>* ActionObject = nullptr;
	if (!Root->TryGetObjectField(TEXT("observation_plan"), ObservationObject) || !ObservationObject || !ObservationObject->IsValid())
	{
		return ArtifactFail(OutError, TEXT("missing required field 'observation_plan'"));
	}
	if (!Root->TryGetObjectField(TEXT("action_plan"), ActionObject) || !ActionObject || !ActionObject->IsValid())
	{
		return ArtifactFail(OutError, TEXT("missing required field 'action_plan'"));
	}

	const TSharedPtr<FJsonObject>* RobotRuntimeObject = nullptr;
	if (!Root->TryGetObjectField(TEXT("robot_runtime"), RobotRuntimeObject)
		|| !RobotRuntimeObject
		|| !RobotRuntimeObject->IsValid())
	{
		return ArtifactFail(OutError, TEXT("missing required field 'robot_runtime'"));
	}

	const TSharedPtr<FJsonObject>* TimingObject = nullptr;
	if (!Root->TryGetObjectField(TEXT("timing"), TimingObject) || !TimingObject || !TimingObject->IsValid())
	{
		return ArtifactFail(OutError, TEXT("missing required field 'timing'"));
	}
	double PhysicsDt = 0.0;
	double DecimationMinNumber = 0.0;
	double DecimationMaxNumber = 0.0;
	if (!(*TimingObject)->TryGetNumberField(TEXT("physics_dt"), PhysicsDt)
		|| !(*TimingObject)->TryGetNumberField(TEXT("decimation_min"), DecimationMinNumber)
		|| !(*TimingObject)->TryGetNumberField(TEXT("decimation_max"), DecimationMaxNumber)
		|| !FMath::IsFinite(PhysicsDt) || PhysicsDt <= 0.0
		|| !FMath::IsFinite(DecimationMinNumber) || !FMath::IsFinite(DecimationMaxNumber)
		|| FMath::FloorToDouble(DecimationMinNumber) != DecimationMinNumber
		|| FMath::FloorToDouble(DecimationMaxNumber) != DecimationMaxNumber
		|| DecimationMinNumber < 1.0 || DecimationMaxNumber < 1.0
		|| DecimationMinNumber > static_cast<double>(TNumericLimits<int32>::Max())
		|| DecimationMaxNumber > static_cast<double>(TNumericLimits<int32>::Max())
		|| DecimationMinNumber > DecimationMaxNumber
		|| !FMath::IsFinite(PhysicsDt * DecimationMaxNumber))
	{
		return ArtifactFail(OutError, TEXT("timing contains invalid values"));
	}
	FUERLPolicyArtifactTiming ParsedTiming;
	ParsedTiming.PhysicsDt = PhysicsDt;
	ParsedTiming.DecimationMin = static_cast<int32>(DecimationMinNumber);
	ParsedTiming.DecimationMax = static_cast<int32>(DecimationMaxNumber);

	const TArray<TSharedPtr<FJsonValue>>* ActuatorsJson = nullptr;
	if (!(*RobotRuntimeObject)->TryGetArrayField(TEXT("actuators"), ActuatorsJson)
		|| ActuatorsJson == nullptr
		|| ActuatorsJson->Num() == 0)
	{
		return ArtifactFail(OutError, TEXT("robot_runtime.actuators must be a non-empty array"));
	}

	FUERLPolicyRobotRuntime ParsedRuntime;
	ParsedRuntime.Actuators.Reserve(ActuatorsJson->Num());
	for (int32 Index = 0; Index < ActuatorsJson->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Entry = (*ActuatorsJson)[Index];
		const TSharedPtr<FJsonObject>* ActuatorObject = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(ActuatorObject) || !ActuatorObject || !ActuatorObject->IsValid())
		{
			return ArtifactFail(
				OutError,
				FString::Printf(TEXT("robot_runtime.actuators[%d] must be an object"), Index));
		}

		FString JointText;
		double Stiffness = 0.0;
		double Damping = 0.0;
		double EffortLimit = 0.0;
		double DefaultPosition = 0.0;
		if (!(*ActuatorObject)->TryGetStringField(TEXT("joint"), JointText) || JointText.IsEmpty()
			|| !(*ActuatorObject)->TryGetNumberField(TEXT("stiffness"), Stiffness)
			|| !(*ActuatorObject)->TryGetNumberField(TEXT("damping"), Damping)
			|| !(*ActuatorObject)->TryGetNumberField(TEXT("effort_limit"), EffortLimit)
			|| !(*ActuatorObject)->TryGetNumberField(TEXT("default_position"), DefaultPosition))
		{
			return ArtifactFail(
				OutError,
				FString::Printf(
					TEXT("robot_runtime.actuators[%d] missing joint/stiffness/damping/effort_limit/default_position"),
					Index));
		}
		if (!FMath::IsFinite(Stiffness) || !FMath::IsFinite(Damping)
			|| !FMath::IsFinite(EffortLimit) || !FMath::IsFinite(DefaultPosition))
		{
			return ArtifactFail(
				OutError,
				FString::Printf(TEXT("robot_runtime.actuators[%d] has non-finite parameter"), Index));
		}

		FUERLPolicyRobotRuntimeActuator& Actuator = ParsedRuntime.Actuators.AddDefaulted_GetRef();
		Actuator.JointName = FName(*JointText);
		Actuator.Stiffness = Stiffness;
		Actuator.Damping = Damping;
		Actuator.EffortLimit = EffortLimit;
		Actuator.DefaultPosition = DefaultPosition;
	}

	FUERLObservationPlan ParsedObservation;
	FUERLActionPlan ParsedAction;
	if (!ParsedObservation.ParseJson(ObservationObject->ToSharedRef(), OutError))
	{
		return false;
	}
	if (!ParsedAction.ParseJson(ActionObject->ToSharedRef(), OutError))
	{
		return false;
	}

	Version = FormatVersion;
	Task = FName(*TaskText);
	Robot = FName(*RobotText);
	Observation = MoveTemp(ParsedObservation);
	Action = MoveTemp(ParsedAction);
	Runtime = MoveTemp(ParsedRuntime);
	ArtifactTiming = ParsedTiming;
	Onnx = MoveTemp(OnnxBytes);
	bLoaded = true;
	OutError.Reset();
	return true;
}

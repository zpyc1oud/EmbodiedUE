#include "UERLPolicyTraceRecorder.h"

#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UERLPolicyLog.h"

namespace
{
	FString QuoteYaml(const FString& Value)
	{
		FString Quoted(TEXT("\""));
		for (const TCHAR Character : Value)
		{
			switch (Character)
			{
			case TEXT('\\'):
				Quoted += TEXT("\\\\");
				break;
			case TEXT('"'):
				Quoted += TEXT("\\\"");
				break;
			case TEXT('\n'):
				Quoted += TEXT("\\n");
				break;
			case TEXT('\r'):
				Quoted += TEXT("\\r");
				break;
			case TEXT('\t'):
				Quoted += TEXT("\\t");
				break;
			default:
				if (Character < 0x20)
				{
					Quoted += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
				}
				else
				{
					Quoted.AppendChar(Character);
				}
			}
		}
		Quoted.AppendChar(TEXT('"'));
		return Quoted;
	}

	FString YamlNumber(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	FString YamlFloatArray(const TArray<float>& Values)
	{
		FString Result(TEXT("["));
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += TEXT(", ");
			}
			Result += YamlNumber(static_cast<double>(Values[Index]));
		}
		Result.AppendChar(TEXT(']'));
		return Result;
	}

	bool IsSafeTraceFileName(const FString& FileName)
	{
		return !FileName.IsEmpty()
			&& FileName != TEXT(".")
			&& FileName != TEXT("..")
			&& !FileName.Contains(TEXT(".."))
			&& !FileName.Contains(TEXT("/"))
			&& !FileName.Contains(TEXT("\\"))
			&& FPaths::GetCleanFilename(FileName) == FileName;
	}

	FString Sha1Hex(TConstArrayView<uint8> Bytes)
	{
		uint8 Hash[20] = {};
		FSHA1::HashBuffer(Bytes.GetData(), static_cast<uint64>(Bytes.Num()), Hash);
		FString Result;
		Result.Reserve(40);
		for (const uint8 Byte : Hash)
		{
			Result += FString::Printf(TEXT("%02x"), static_cast<uint32>(Byte));
		}
		return Result;
	}

	FString NormalizeMapPackage(const FString& PackageName)
	{
		int32 LastSlash = INDEX_NONE;
		PackageName.FindLastChar(TEXT('/'), LastSlash);
		const int32 LeafStart = LastSlash + 1;
		if (!PackageName.Mid(LeafStart).StartsWith(TEXT("UEDPIE_")))
		{
			return PackageName;
		}

		const int32 PrefixNumberStart = LeafStart + 7;
		const int32 PrefixEnd = PackageName.Find(
			TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, PrefixNumberStart);
		if (PrefixEnd <= PrefixNumberStart)
		{
			return PackageName;
		}
		for (int32 Index = PrefixNumberStart; Index < PrefixEnd; ++Index)
		{
			if (!FChar::IsDigit(PackageName[Index]))
			{
				return PackageName;
			}
		}
		return PackageName.Left(LeafStart) + PackageName.Mid(PrefixEnd + 1);
	}
}

UUERLPolicyTraceRecorder::UUERLPolicyTraceRecorder()
{
	PrimaryComponentTick.bCanEverTick = false;
	PrimaryComponentTick.bStartWithTickEnabled = false;
}

bool UUERLPolicyTraceRecorder::StartTrace(const FString& FileName, int32 InSeed)
{
	if (bRecording)
	{
		LastError = TEXT("a policy trace is already recording");
		return false;
	}
	if (!TracePath.IsEmpty() && !bTraceFinalized)
	{
		LastError = TEXT("the previous trace is not finalized; call StopTrace again before starting another");
		return false;
	}
	TracePath.Reset();
	BoundPolicyComponent = nullptr;
	bTraceRequestedComplete = false;
	bTraceFinalized = false;
	bTraceFinalizationBlocked = false;
	bSawPolicyFault = false;
	LastError.Reset();
	if (!IsSafeTraceFileName(FileName))
	{
		LastError = TEXT("trace file name must be a simple file name under Saved/UERLPolicyTraces");
		return false;
	}
	if (InSeed < -1)
	{
		LastError = TEXT("seed must be -1 when unknown or a non-negative value");
		return false;
	}
	if (!PolicyComponent)
	{
		if (AActor* Owner = GetOwner())
		{
			PolicyComponent = Owner->FindComponentByClass<UUERLPolicyComponent>();
		}
	}
	if (!PolicyComponent || !PolicyComponent->Artifact)
	{
		LastError = TEXT("trace recorder requires an owner PolicyComponent with an Artifact");
		return false;
	}
	FUERLPolicyArtifact ParsedArtifact;
	FString ArtifactError;
	if (!PolicyComponent->Artifact->LoadArtifact(ParsedArtifact, ArtifactError))
	{
		LastError = FString::Printf(TEXT("could not load policy artifact for trace identity: %s"), *ArtifactError);
		return false;
	}
	if (!GetWorld())
	{
		LastError = TEXT("trace recorder has no World");
		return false;
	}
	const FString Extension = FPaths::GetExtension(FileName, true);
	if (!Extension.IsEmpty() && !Extension.Equals(TEXT(".yaml"), ESearchCase::IgnoreCase))
	{
		LastError = TEXT("trace file extension must be .yaml");
		return false;
	}
	const FString FinalFileName = Extension.IsEmpty() ? FileName + TEXT(".yaml") : FileName;
	const FString TraceDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UERLPolicyTraces"));
	const FString FinalPath = FPaths::Combine(TraceDirectory, FinalFileName);
	if (IFileManager::Get().FileExists(*FinalPath))
	{
		LastError = FString::Printf(TEXT("trace already exists and will not be overwritten: %s"), *FinalPath);
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*TraceDirectory, true))
	{
		LastError = FString::Printf(TEXT("could not create trace directory: %s"), *TraceDirectory);
		return false;
	}

	TracePath = FinalPath;
	Seed = InSeed;
	TaskId = PolicyComponent->Artifact->Summary.TaskId.ToString();
	RobotId = PolicyComponent->Artifact->Summary.RobotId.ToString();
	PolicyOnnxSha1 = Sha1Hex(ParsedArtifact.OnnxBytes());
	ArtifactAssetPath = PolicyComponent->Artifact->GetPathName();
	MapPackagePath = NormalizeMapPackage(GetWorld()->GetOutermost()->GetName());
	PhysicsDtSeconds = PolicyComponent->Artifact->Summary.PhysicsDt;
	DecimationMin = PolicyComponent->Artifact->Summary.DecimationMin;
	DecimationMax = PolicyComponent->Artifact->Summary.DecimationMax;
	Records.Reset();
	RawStateFields.Reset();
	ObservationWidth = 0;
	PreviousActionWidth = 0;
	ActionWidth = 0;
	ActuatorTargetWidth = 0;
	EpisodeIndex = 0;
	EpisodeStep = 0;
	PendingEpisodeIndex = 0;
	LastSequence = PolicyComponent->GetControlFrameSequence();
	PendingBoundaryAfterSequence = 0;
	EpisodeElapsedSeconds = 0.0;
	PendingBoundaryReason.Reset();
	bSawFirstFrame = false;
	bPendingEpisodeBoundary = false;
	bRecording = true;
	BoundPolicyComponent = PolicyComponent;
	BoundPolicyComponent->OnControlStepCompleted.AddUniqueDynamic(
		this, &UUERLPolicyTraceRecorder::OnControlFrameCompleted);
	BoundPolicyComponent->OnControlStepOverrun.AddUniqueDynamic(
		this, &UUERLPolicyTraceRecorder::OnControlStepOverrun);
	BoundPolicyComponent->OnCommandStale.AddUniqueDynamic(
		this, &UUERLPolicyTraceRecorder::OnCommandStale);
	BoundPolicyComponent->OnPolicyFault.AddUniqueDynamic(
		this, &UUERLPolicyTraceRecorder::OnPolicyFault);
	return true;
}

bool UUERLPolicyTraceRecorder::MarkEpisodeBoundary(int32 NewEpisodeIndex, const FString& Reason)
{
	if (!bRecording)
	{
		LastError = TEXT("no policy trace is recording");
		return false;
	}
	if (bPendingEpisodeBoundary)
	{
		LastError = TEXT("an episode boundary is already pending for the next bootstrap frame");
		return false;
	}
	if (NewEpisodeIndex <= EpisodeIndex)
	{
		LastError = FString::Printf(
			TEXT("new episode index %d must be greater than current index %d"), NewEpisodeIndex, EpisodeIndex);
		return false;
	}
	if (!BoundPolicyComponent || !BoundPolicyComponent->IsBootstrapPending())
	{
		LastError = TEXT("mark the episode boundary after reset/restart arms the next bootstrap frame");
		return false;
	}
	PendingEpisodeIndex = NewEpisodeIndex;
	PendingBoundaryAfterSequence = BoundPolicyComponent->GetControlFrameSequence();
	PendingBoundaryReason = Reason;
	bPendingEpisodeBoundary = true;
	LastError.Reset();
	return true;
}

bool UUERLPolicyTraceRecorder::StopTrace()
{
	return FinalizeTrace(true);
}

bool UUERLPolicyTraceRecorder::FinalizeTrace(bool bMarkComplete)
{
	if (bTraceFinalizationBlocked)
	{
		return false;
	}
	if (bTraceFinalized || TracePath.IsEmpty())
	{
		return true;
	}
	if (bMarkComplete)
	{
		bTraceRequestedComplete = true;
	}
	if (bRecording)
	{
		bRecording = false;
		if (BoundPolicyComponent)
		{
			BoundPolicyComponent->OnControlStepCompleted.RemoveDynamic(
				this, &UUERLPolicyTraceRecorder::OnControlFrameCompleted);
			BoundPolicyComponent->OnControlStepOverrun.RemoveDynamic(
				this, &UUERLPolicyTraceRecorder::OnControlStepOverrun);
			BoundPolicyComponent->OnCommandStale.RemoveDynamic(
				this, &UUERLPolicyTraceRecorder::OnCommandStale);
			BoundPolicyComponent->OnPolicyFault.RemoveDynamic(
				this, &UUERLPolicyTraceRecorder::OnPolicyFault);
		}
	}
	return WriteTrace();
}

bool UUERLPolicyTraceRecorder::WriteTrace()
{
	if (IFileManager::Get().FileExists(*TracePath))
	{
		bTraceFinalizationBlocked = true;
		LastError = FString::Printf(TEXT("trace appeared before flush; refusing to overwrite: %s"), *TracePath);
		return false;
	}
	const FString Contents = SerializeTrace();
	const FString TemporaryPath = TracePath + TEXT(".")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(
		Contents, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		LastError = FString::Printf(TEXT("could not write temporary policy trace: %s"), *TemporaryPath);
		return false;
	}
	if (!IFileManager::Get().Move(*TracePath, *TemporaryPath, false, false, false, true))
	{
		IFileManager::Get().Delete(*TemporaryPath, false, true, true);
		bTraceFinalizationBlocked = IFileManager::Get().FileExists(*TracePath);
		LastError = FString::Printf(TEXT("could not publish policy trace without replacing an existing file: %s"), *TracePath);
		return false;
	}
	bTraceFinalized = true;
	LastError.Reset();
	return true;
}

void UUERLPolicyTraceRecorder::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (!FinalizeTrace(false) && !bTraceFinalizationBlocked)
	{
		UE_LOG(LogUERLPolicy, Error, TEXT("[UERLPolicyTraceRecorder] EndPlay trace flush failed: %s"), *LastError);
	}
	Super::EndPlay(EndPlayReason);
}

void UUERLPolicyTraceRecorder::OnControlFrameCompleted(const FUERLPolicyControlFrameSnapshot& Frame)
{
	if (!bRecording)
	{
		return;
	}
	FString Phase;
	const bool bApplyEpisodeBoundary = bPendingEpisodeBoundary
		&& Frame.Sequence > PendingBoundaryAfterSequence
		&& Frame.bBootstrap;
	if (bApplyEpisodeBoundary)
	{
		EpisodeIndex = PendingEpisodeIndex;
		EpisodeStep = 0;
		EpisodeElapsedSeconds = 0.0;
		Phase = TEXT("post_reset_input");
		bPendingEpisodeBoundary = false;
		FString BoundaryRecord(TEXT("  - kind: boundary\n"));
		BoundaryRecord += FString::Printf(
			TEXT("    sequence: %lld\n"), static_cast<long long>(Frame.Sequence));
		BoundaryRecord += FString::Printf(TEXT("    episode_index: %d\n"), EpisodeIndex);
		BoundaryRecord += FString::Printf(TEXT("    reason: %s\n"), *QuoteYaml(PendingBoundaryReason));
		Records.Add(MoveTemp(BoundaryRecord));
		PendingBoundaryReason.Reset();
	}
	else if (Frame.bBootstrap)
	{
		EpisodeElapsedSeconds = 0.0;
		Phase = TEXT("bootstrap_input");
		if (bSawFirstFrame && !bPendingEpisodeBoundary)
		{
			FString EventRecord(TEXT("  - kind: event\n"));
			EventRecord += TEXT("    event: unmarked_bootstrap_boundary\n");
			EventRecord += FString::Printf(TEXT("    sequence: %lld\n"), static_cast<long long>(Frame.Sequence));
			EventRecord += FString::Printf(TEXT("    episode_index: %d\n"), EpisodeIndex);
			EventRecord += FString::Printf(TEXT("    episode_step: %d\n"), EpisodeStep);
			Records.Add(MoveTemp(EventRecord));
		}
		else
		{
			EpisodeStep = 0;
		}
	}
	else
	{
		EpisodeElapsedSeconds += Frame.PhysicsElapsedSeconds;
		Phase = TEXT("post_window_input");
	}
	LastSequence = Frame.Sequence;

	if (RawStateFields.IsEmpty())
	{
		RawStateFields = Frame.RawStateFields;
		ObservationWidth = Frame.Observation.Num();
		PreviousActionWidth = Frame.PreviousAction.Num();
		ActionWidth = Frame.Action.Num();
		ActuatorTargetWidth = Frame.ActuatorTargets.Num();
	}
	FString Record(TEXT("  - kind: frame\n"));
	Record += FString::Printf(TEXT("    sequence: %lld\n"), static_cast<long long>(Frame.Sequence));
	Record += FString::Printf(TEXT("    episode_index: %d\n"), EpisodeIndex);
	Record += FString::Printf(TEXT("    episode_step: %d\n"), EpisodeStep);
	Record += FString::Printf(TEXT("    phase: %s\n"), *Phase);
	Record += TEXT("    clocks:\n");
	Record += FString::Printf(TEXT("      solver_time_s: %s\n"), *YamlNumber(Frame.SolverTimeSeconds));
	Record += FString::Printf(TEXT("      episode_elapsed_s: %s\n"), *YamlNumber(EpisodeElapsedSeconds));
	Record += FString::Printf(TEXT("      physics_elapsed_s: %s\n"), *YamlNumber(Frame.PhysicsElapsedSeconds));
	Record += FString::Printf(TEXT("      game_elapsed_s: %s\n"), *YamlNumber(Frame.GameElapsedSeconds));
	Record += FString::Printf(TEXT("      observation_dt_s: %s\n"), *YamlNumber(Frame.ObservationDtSeconds));
	Record += FString::Printf(TEXT("      last_solver_step_dt_s: %s\n"), *YamlNumber(Frame.LastSolverStepSeconds));
	Record += TEXT("    input:\n");
	Record += TEXT("      raw_state_fields:\n");
	for (const FUERLPolicyStateFieldSample& Field : Frame.RawStateFields)
	{
		Record += FString::Printf(TEXT("        - name: %s\n"), *QuoteYaml(Field.Name.ToString()));
		Record += FString::Printf(TEXT("          width: %d\n"), Field.Width);
	}
	Record += FString::Printf(TEXT("      raw_state: %s\n"), *YamlFloatArray(Frame.RawState));
	Record += FString::Printf(TEXT("      observation: %s\n"), *YamlFloatArray(Frame.Observation));
	Record += FString::Printf(TEXT("      previous_action: %s\n"), *YamlFloatArray(Frame.PreviousAction));
	if (Frame.Commands.IsEmpty())
	{
		Record += TEXT("      commands: []\n");
	}
	else
	{
		Record += TEXT("      commands:\n");
		for (const FUERLPolicyCommandSample& Command : Frame.Commands)
		{
			Record += FString::Printf(TEXT("        - channel: %s\n"), *QuoteYaml(Command.Channel.ToString()));
			Record += FString::Printf(TEXT("          values: %s\n"), *YamlFloatArray(Command.Values));
			Record += FString::Printf(TEXT("          age_seconds: %s\n"), *YamlNumber(Command.AgeSeconds));
		}
	}
	Record += TEXT("    action:\n");
	Record += FString::Printf(TEXT("      policy_action: %s\n"), *YamlFloatArray(Frame.Action));
	Record += FString::Printf(TEXT("      actuator_targets: %s\n"), *YamlFloatArray(Frame.ActuatorTargets));
	Records.Add(MoveTemp(Record));
	++EpisodeStep;
	bSawFirstFrame = true;
}

void UUERLPolicyTraceRecorder::OnControlStepOverrun(
	float GameSeconds,
	float PhysicsSeconds,
	float ObservationSeconds)
{
	if (!bRecording)
	{
		return;
	}
	FString Record(TEXT("  - kind: event\n"));
	Record += TEXT("    event: control_step_overrun\n");
	const int64 EventSequence = bPendingEpisodeBoundary
		? PendingBoundaryAfterSequence + 1 : LastSequence + 1;
	Record += FString::Printf(TEXT("    sequence: %lld\n"), static_cast<long long>(EventSequence));
	Record += FString::Printf(
		TEXT("    episode_index: %d\n"), bPendingEpisodeBoundary ? PendingEpisodeIndex : EpisodeIndex);
	Record += FString::Printf(TEXT("    episode_step: %d\n"), bPendingEpisodeBoundary ? 0 : EpisodeStep);
	Record += FString::Printf(TEXT("    game_seconds: %s\n"), *YamlNumber(GameSeconds));
	Record += FString::Printf(TEXT("    physics_seconds: %s\n"), *YamlNumber(PhysicsSeconds));
	Record += FString::Printf(TEXT("    observation_seconds: %s\n"), *YamlNumber(ObservationSeconds));
	Records.Add(MoveTemp(Record));
}

void UUERLPolicyTraceRecorder::OnCommandStale(FName Channel, float StaleSeconds)
{
	if (!bRecording)
	{
		return;
	}
	FString Record(TEXT("  - kind: event\n"));
	Record += TEXT("    event: command_stale\n");
	const int64 EventSequence = bPendingEpisodeBoundary
		? PendingBoundaryAfterSequence + 1 : LastSequence + 1;
	Record += FString::Printf(TEXT("    sequence: %lld\n"), static_cast<long long>(EventSequence));
	Record += FString::Printf(
		TEXT("    episode_index: %d\n"), bPendingEpisodeBoundary ? PendingEpisodeIndex : EpisodeIndex);
	Record += FString::Printf(TEXT("    episode_step: %d\n"), bPendingEpisodeBoundary ? 0 : EpisodeStep);
	Record += FString::Printf(TEXT("    channel: %s\n"), *QuoteYaml(Channel.ToString()));
	Record += FString::Printf(TEXT("    stale_seconds: %s\n"), *YamlNumber(StaleSeconds));
	Records.Add(MoveTemp(Record));
}

void UUERLPolicyTraceRecorder::OnPolicyFault(const FString& Reason)
{
	if (!bRecording)
	{
		return;
	}
	bSawPolicyFault = true;
	FString Record(TEXT("  - kind: event\n"));
	Record += TEXT("    event: policy_fault\n");
	const int64 EventSequence = bPendingEpisodeBoundary
		? PendingBoundaryAfterSequence + 1 : LastSequence + 1;
	Record += FString::Printf(TEXT("    sequence: %lld\n"), static_cast<long long>(EventSequence));
	Record += FString::Printf(
		TEXT("    episode_index: %d\n"), bPendingEpisodeBoundary ? PendingEpisodeIndex : EpisodeIndex);
	Record += FString::Printf(TEXT("    episode_step: %d\n"), bPendingEpisodeBoundary ? 0 : EpisodeStep);
	Record += FString::Printf(TEXT("    reason: %s\n"), *QuoteYaml(Reason));
	Records.Add(MoveTemp(Record));
}

FString UUERLPolicyTraceRecorder::SerializeTrace() const
{
	FString Result(TEXT("schema_version: 1\nsource:\n"));
	Result += TEXT("  side: ue\n");
	Result += FString::Printf(TEXT("  task_id: %s\n"), *QuoteYaml(TaskId));
	Result += FString::Printf(TEXT("  robot_id: %s\n"), *QuoteYaml(RobotId));
	Result += FString::Printf(TEXT("  policy_onnx_sha1: %s\n"), *QuoteYaml(PolicyOnnxSha1));
	Result += FString::Printf(TEXT("  artifact_asset: %s\n"), *QuoteYaml(ArtifactAssetPath));
	Result += FString::Printf(TEXT("  map_package: %s\n"), *QuoteYaml(MapPackagePath));
	Result += Seed < 0
		? TEXT("  seed: null\n")
		: FString::Printf(TEXT("  seed: %d\n"), Seed);
	Result += TEXT("clock:\n");
	Result += FString::Printf(TEXT("  physics_dt_s: %s\n"), *YamlNumber(PhysicsDtSeconds));
	Result += FString::Printf(TEXT("  decimation_min: %d\n"), DecimationMin);
	Result += FString::Printf(TEXT("  decimation_max: %d\n"), DecimationMax);
	Result += TEXT("field_layout:\n  raw_state_fields:\n");
	for (const FUERLPolicyStateFieldSample& Field : RawStateFields)
	{
		Result += FString::Printf(TEXT("    - name: %s\n"), *QuoteYaml(Field.Name.ToString()));
		Result += FString::Printf(TEXT("      width: %d\n"), Field.Width);
	}
	if (RawStateFields.IsEmpty())
	{
		Result += TEXT("    []\n");
	}
	Result += FString::Printf(TEXT("  observation_width: %d\n"), ObservationWidth);
	Result += FString::Printf(TEXT("  previous_action_width: %d\n"), PreviousActionWidth);
	Result += FString::Printf(TEXT("  action_width: %d\n"), ActionWidth);
	Result += FString::Printf(TEXT("  actuator_target_width: %d\n"), ActuatorTargetWidth);
	const TCHAR* Status = bSawPolicyFault
		? TEXT("faulted")
		: bTraceRequestedComplete ? TEXT("complete") : TEXT("incomplete");
	Result += FString::Printf(TEXT("status: %s\nrecords:\n"), Status);
	if (Records.IsEmpty())
	{
		Result += TEXT("  []\n");
	}
	else
	{
		for (const FString& Record : Records)
		{
			Result += Record;
		}
	}
	return Result;
}

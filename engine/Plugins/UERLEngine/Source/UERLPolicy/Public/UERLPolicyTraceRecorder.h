#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UERLPolicyComponent.h"
#include "UERLPolicyTraceRecorder.generated.h"

/** Explicit host-owned trace writer for paired Task/UE deployment diagnostics. */
UCLASS(ClassGroup = (UERL), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class UERLPOLICY_API UUERLPolicyTraceRecorder final : public UActorComponent
{
	GENERATED_BODY()

public:
	UUERLPolicyTraceRecorder();

	/** Optional explicit component; if unset, StartTrace finds one on the owner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Policy|Trace")
	TObjectPtr<UUERLPolicyComponent> PolicyComponent = nullptr;

	/** Write a fresh YAML file under Saved/UERLPolicyTraces using this file name. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy|Trace")
	bool StartTrace(const FString& FileName, int32 Seed = -1);

	/** Mark a host reset/restart before the next policy bootstrap frame. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy|Trace")
	bool MarkEpisodeBoundary(int32 NewEpisodeIndex, const FString& Reason);

	/** Flush and close the current trace. Existing files are never overwritten. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy|Trace")
	bool StopTrace();

	UFUNCTION(BlueprintPure, Category = "UERL|Policy|Trace")
	bool IsRecording() const { return bRecording; }

	UFUNCTION(BlueprintPure, Category = "UERL|Policy|Trace")
	FString GetLastError() const { return LastError; }

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	bool FinalizeTrace(bool bMarkComplete);
	bool WriteTrace();
	void AppendEventPosition(FString& Record) const;

	UFUNCTION()
	void OnControlFrameCompleted(const FUERLPolicyControlFrameSnapshot& Frame);

	UFUNCTION()
	void OnControlStepOverrun(float GameSeconds, float PhysicsSeconds, float ObservationSeconds);

	UFUNCTION()
	void OnCommandStale(FName Channel, float StaleSeconds);

	UFUNCTION()
	void OnPolicyFault(FString Reason);

	FString SerializeTrace() const;
	FString TracePath;
	FString LastError;
	UPROPERTY(Transient)
	TObjectPtr<UUERLPolicyComponent> BoundPolicyComponent = nullptr;
	FString TaskId;
	FString RobotId;
	FString PolicyOnnxSha1;
	FString ArtifactAssetPath;
	FString MapPackagePath;
	TArray<FString> Records;
	TArray<FUERLPolicyStateFieldSample> RawStateFields;
	double PhysicsDtSeconds = 0.0;
	int32 DecimationMin = 0;
	int32 DecimationMax = 0;
	int32 ObservationWidth = 0;
	int32 PreviousActionWidth = 0;
	int32 ActionWidth = 0;
	int32 ActuatorTargetWidth = 0;
	int32 Seed = -1;
	int32 EpisodeIndex = 0;
	int32 EpisodeStep = 0;
	int32 PendingEpisodeIndex = 0;
	int64 LastSequence = 0;
	int64 PendingBoundaryAfterSequence = 0;
	double EpisodeElapsedSeconds = 0.0;
	FString PendingBoundaryReason;
	bool bRecording = false;
	bool bSawFirstFrame = false;
	bool bPendingEpisodeBoundary = false;
	bool bSawPolicyFault = false;
	bool bTraceRequestedComplete = false;
	bool bTraceFinalized = false;
	bool bTraceFinalizationBlocked = false;
};

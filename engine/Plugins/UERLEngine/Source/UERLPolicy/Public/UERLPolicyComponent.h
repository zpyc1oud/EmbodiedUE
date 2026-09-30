#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UERLPolicyArtifactAsset.h"
#include "UERLPolicyController.h"
#include "UERLPolicyComponent.generated.h"

class USkeletalMeshComponent;

/** Blueprint-safe command channel description derived from the artifact plan. */
USTRUCT(BlueprintType)
struct UERLPOLICY_API FUERLPolicyCommandChannelInfo
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	FName Name;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	int32 Width = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FUERLPolicyControlStepOverrunSignature,
	float,
	GameSeconds,
	float,
	PhysicsSeconds,
	float,
	ObservationSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FUERLPolicyCommandStaleSignature,
	FName,
	Channel,
	float,
	StaleSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUERLPolicyFaultSignature, FString, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FUERLPolicyPhysicsBaselineMismatchSignature,
	FString,
	Report);

/** Stable in-game policy component. Start/Stop are explicit and per-instance. */
UCLASS(ClassGroup = (UERL), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class UERLPOLICY_API UUERLPolicyComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UUERLPolicyComponent();

	/** Cooked policy bytes and the explicit mesh dependency used by this instance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Policy")
	TObjectPtr<UUERLPolicyArtifactAsset> Artifact = nullptr;

	/** Claim one authored mesh on the owner instead of spawning a new Actor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Policy")
	bool bClaimOwnerMesh = true;

	/** Optional explicit owner mesh; required when an owner has multiple matching meshes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Policy")
	TObjectPtr<USkeletalMeshComponent> OwnerMesh = nullptr;

	/** Zero disables stale-command diagnostics. This is not a control-period setting. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Policy", meta = (ClampMin = "0.0"))
	double CommandStalenessSeconds = 0.0;

	/** Start on the first tick after BeginPlay. Turn off to bind events and SetCommand, then call StartPolicy. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Policy")
	bool bAutoStart = true;

	UPROPERTY(BlueprintAssignable, Category = "Policy|Events")
	FUERLPolicyControlStepOverrunSignature OnControlStepOverrun;

	UPROPERTY(BlueprintAssignable, Category = "Policy|Events")
	FUERLPolicyCommandStaleSignature OnCommandStale;

	UPROPERTY(BlueprintAssignable, Category = "Policy|Events")
	FUERLPolicyFaultSignature OnPolicyFault;

	UPROPERTY(BlueprintAssignable, Category = "Policy|Events")
	FUERLPolicyPhysicsBaselineMismatchSignature OnPhysicsBaselineMismatch;

	/** Load, validate, claim/spawn, and arm the first PostPhysics bootstrap. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy")
	bool StartPolicy();

	/** Stop inference and release only resources owned by this component. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy")
	void StopPolicy();

	/** Latch one host command channel; values are copied per component instance. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy")
	bool SetCommand(FName Channel, const TArray<float>& Values);

	/** Return command names and widths derived from the artifact observation plan. */
	UFUNCTION(BlueprintPure, Category = "UERL|Policy")
	TArray<FUERLPolicyCommandChannelInfo> GetRequiredCommandChannels() const;

	UFUNCTION(BlueprintPure, Category = "UERL|Policy")
	bool IsRunning() const { return bRunning; }

	UFUNCTION(BlueprintPure, Category = "UERL|Policy")
	FString GetLastError() const { return LastError; }

	/** Live skeletal mesh transform after Start. Not the component owner. */
	UFUNCTION(BlueprintPure, Category = "UERL|Policy")
	bool GetRobotTransform(FTransform& OutTransform) const;

	/** Internal timing probe used by integration tests and diagnostics. */
	const FUERLControlTiming& GetLastControlTiming() const { return Controller.LastControlTiming(); }

	/** Clear policy history, contacts, terrain caches, and solver accumulation. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy")
	bool SoftReset();

	/** Reset the live robot pose without destroying a spawned Actor. */
	UFUNCTION(BlueprintCallable, Category = "UERL|Policy")
	bool ResetToReferencePose();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool ResolveOwnerMesh(USkeletalMeshComponent*& OutMesh, FString& OutError) const;
	bool StartFailure(const FString& Error, bool bPhysicsMismatch = false);
	void Fault(const FString& Error);
	void RefreshSolverBaseline();
	void ResetCommandAges();
	/** Re-arm the running state after Start/SoftReset/ResetToReferencePose. */
	void RearmPolicyLoop();

	FUERLPolicyController Controller;
	TMap<FName, TArray<float>> LatchedCommands;
	TMap<FName, double> CommandAges;
	TSet<FName> StaleChannels;
	bool bRunning = false;
	bool bBootstrapPending = false;
	bool bFaulted = false;
	int32 LastSolverFrame = INDEX_NONE;
	double LastSolverTime = 0.0;
	double AccumulatedPhysicsSeconds = 0.0;
	double AccumulatedGameSeconds = 0.0;
	uint64 LifecycleGeneration = 0;
	FString LastError;
	FString LastLoggedSetCommandError;
};

#include "UERLPolicyComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UERLPhysicsSnapshot.h"
#include "UERLPolicyLog.h"
#include "UERLPolicyPhysicsGate.h"

UUERLPolicyComponent::UUERLPolicyComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	PrimaryComponentTick.TickInterval = 0.0f;
	SetComponentTickEnabled(false);
}

void UUERLPolicyComponent::BeginPlay()
{
	Super::BeginPlay();
	SetComponentTickEnabled(bAutoStart);
}

void UUERLPolicyComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopPolicy();
	Super::EndPlay(EndPlayReason);
}

namespace
{
	bool SameSkeletalMesh(const USkeletalMesh* Left, const USkeletalMesh* Right)
	{
		return Left && Right && (Left == Right || Left->GetPathName() == Right->GetPathName());
	}

	FName ComponentBaseName(const UActorComponent& Component)
	{
		FString Name = Component.GetName();
		Name.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
		return FName(*Name);
	}

	bool ComponentUsesRobotMesh(const USkeletalMeshComponent& Component, const USkeletalMesh& RobotMesh)
	{
		return SameSkeletalMesh(Component.GetSkeletalMeshAsset(), &RobotMesh);
	}

	bool IsLiveForeignMesh(const USkeletalMeshComponent& Mesh, const AActor& Owner)
	{
		const AActor* MeshOwner = Mesh.GetOwner();
		return MeshOwner && MeshOwner != &Owner && MeshOwner->GetWorld() != nullptr;
	}
}

bool UUERLPolicyComponent::ResolveOwnerMesh(USkeletalMeshComponent*& OutMesh, FString& OutError) const
{
	OutMesh = nullptr;
	OutError.Reset();
	if (!Artifact || !Artifact->RobotMesh)
	{
		OutError = TEXT("policy component requires an Artifact with RobotMesh");
		return false;
	}
	if (!bClaimOwnerMesh)
	{
		return true;
	}
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		OutError = TEXT("policy component has no owning Actor for mesh claim");
		return false;
	}
	if (OwnerMesh)
	{
		if (IsLiveForeignMesh(*OwnerMesh, *Owner))
		{
			OutError = FString::Printf(
				TEXT("explicit OwnerMesh does not belong to the owner (ownerMesh=%s meshOwner=%s owner=%s)"),
				*GetNameSafe(OwnerMesh),
				*GetNameSafe(OwnerMesh->GetOwner()),
				*GetNameSafe(Owner));
			return false;
		}
		USkeletalMeshComponent* Resolved = nullptr;
		if (OwnerMesh->GetOwner() == Owner && ComponentUsesRobotMesh(*OwnerMesh, *Artifact->RobotMesh))
		{
			Resolved = OwnerMesh;
		}
		else
		{
			const FName WantedName = ComponentBaseName(*OwnerMesh);
			TInlineComponentArray<USkeletalMeshComponent*> Candidates(Owner);
			USkeletalMeshComponent* ByName = nullptr;
			USkeletalMeshComponent* UniqueMatch = nullptr;
			int32 MatchCount = 0;
			for (USkeletalMeshComponent* Candidate : Candidates)
			{
				if (!Candidate)
				{
					continue;
				}
				if (ComponentBaseName(*Candidate) == WantedName)
				{
					ByName = Candidate;
				}
				if (ComponentUsesRobotMesh(*Candidate, *Artifact->RobotMesh))
				{
					++MatchCount;
					UniqueMatch = Candidate;
				}
			}
			if (ByName && ComponentUsesRobotMesh(*ByName, *Artifact->RobotMesh))
			{
				Resolved = ByName;
			}
			else if (MatchCount == 1)
			{
				Resolved = UniqueMatch;
			}
		}
		if (!Resolved || Resolved->GetOwner() != Owner)
		{
			OutError = FString::Printf(
				TEXT("explicit OwnerMesh does not belong to the owner (ownerMesh=%s meshOwner=%s owner=%s)"),
				*GetNameSafe(OwnerMesh),
				*GetNameSafe(OwnerMesh->GetOwner()),
				*GetNameSafe(Owner));
			return false;
		}
		if (!ComponentUsesRobotMesh(*Resolved, *Artifact->RobotMesh))
		{
			OutError = FString::Printf(
				TEXT("explicit OwnerMesh skeletal mesh does not match Artifact RobotMesh (mesh=%s artifact=%s)"),
				*GetPathNameSafe(Resolved->GetSkeletalMeshAsset()),
				*GetPathNameSafe(Artifact->RobotMesh));
			return false;
		}
		OutMesh = Resolved;
		return true;
	}
	TInlineComponentArray<USkeletalMeshComponent*> Candidates(Owner);
	for (USkeletalMeshComponent* Candidate : Candidates)
	{
		if (Candidate && ComponentUsesRobotMesh(*Candidate, *Artifact->RobotMesh))
		{
			if (OutMesh)
			{
				OutError = TEXT("owner has multiple matching SkeletalMesh components; set OwnerMesh explicitly");
				return false;
			}
			OutMesh = Candidate;
		}
	}
	if (!OutMesh)
	{
		OutError = TEXT("owner has no SkeletalMesh component matching Artifact RobotMesh");
		return false;
	}
	return true;
}

bool UUERLPolicyComponent::StartFailure(const FString& Error, bool bPhysicsMismatch)
{
	LastError = Error;
	UE_LOG(LogUERLPolicy, Error, TEXT("[UERLPolicyComponent] StartPolicy failed: %s"), *Error);
	if (bPhysicsMismatch)
	{
		OnPhysicsBaselineMismatch.Broadcast(Error);
	}
	OnPolicyFault.Broadcast(Error);
	return false;
}

void UUERLPolicyComponent::Fault(const FString& Error)
{
	if (bFaulted)
	{
		return;
	}
	bFaulted = true;
	bRunning = false;
	bBootstrapPending = false;
	LastError = Error;
	SetComponentTickEnabled(false);
	OnPolicyFault.Broadcast(Error);
}

void UUERLPolicyComponent::RefreshSolverBaseline()
{
	LastSolverFrame = INDEX_NONE;
	LastSolverTime = 0.0;
	if (UWorld* World = GetWorld())
	{
		FUERLSolverClockSnapshot Clock;
		FString Error;
		if (ReadUERLSolverClock(*World, Clock, Error))
		{
			LastSolverFrame = Clock.Frame;
			LastSolverTime = Clock.SolverTime;
		}
	}
}

void UUERLPolicyComponent::ResetCommandAges()
{
	for (const TPair<FName, TArray<float>>& Pair : LatchedCommands)
	{
		CommandAges.Add(Pair.Key, 0.0);
	}
	StaleChannels.Reset();
}

void UUERLPolicyComponent::RearmPolicyLoop()
{
	++LifecycleGeneration;
	bRunning = true;
	bFaulted = false;
	bBootstrapPending = true;
	AccumulatedPhysicsSeconds = 0.0;
	AccumulatedGameSeconds = 0.0;
	RefreshSolverBaseline();
	ResetCommandAges();
	SetComponentTickEnabled(true);
}

bool UUERLPolicyComponent::StartPolicy()
{
	if (bRunning)
	{
		return true;
	}
	LastError.Reset();
	bFaulted = false;
	USkeletalMeshComponent* ClaimedMesh = nullptr;
	FString Error;
	FUERLPolicyArtifact ParsedArtifact;
	if (!Artifact || !Artifact->LoadArtifact(ParsedArtifact, Error))
	{
		if (Error.IsEmpty())
		{
			Error = TEXT("policy Artifact is invalid");
		}
		return StartFailure(Error);
	}
	if (!Artifact->ValidateRobotMesh(Error))
	{
		return StartFailure(Error);
	}
	if (!ResolveOwnerMesh(ClaimedMesh, Error))
	{
		return StartFailure(Error);
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return StartFailure(TEXT("policy component has no World"));
	}
	FUERLPhysicsSnapshot Snapshot;
	if (!ReadUERLPhysicsSnapshot(*World, GetOwner(), Snapshot, Error))
	{
		return StartFailure(Error, true);
	}
	const FUERLPhysicsGateReport Gate = CheckDeployPhysicsRequirements(Snapshot, ParsedArtifact.Timing());
	if (!Gate.bPassed)
	{
		return StartFailure(Gate.ToString(), true);
	}

	FUERLPolicyControllerConfig Config;
	Config.AssetPath = Artifact->RobotMesh->GetPathName();
	Config.GroundOrigin = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
	Config.GroundNormal = FVector::UpVector;
	Config.bClaimAuthoredActor = bClaimOwnerMesh;
	Config.ClaimedMesh = ClaimedMesh;
	Config.PlacementTransform = GetOwner() ? GetOwner()->GetActorTransform() : FTransform::Identity;
	Config.bHasPlacementTransform = !bClaimOwnerMesh;
	const FVector ProbeLocation = ClaimedMesh
		? ClaimedMesh->GetComponentLocation()
		: Config.PlacementTransform.GetLocation();
	if (!MeasureUERLWorldStaticClearanceMeters(
			*World,
			ProbeLocation,
			Config.GroundNormal,
			Config.InitialRootHeightMeters,
			Error,
			GetOwner()))
	{
		return StartFailure(Error);
	}
	Config.GroundOrigin = ProbeLocation - Config.GroundNormal * (Config.InitialRootHeightMeters * 100.0);
	if (!Controller.InitializeFromBytes(*World, Artifact->ArtifactBytes, Config, Error))
	{
		return StartFailure(Error);
	}
	RearmPolicyLoop();
	UE_LOG(LogUERLPolicy, Display, TEXT("[UERLPolicyComponent] StartPolicy succeeded owner=%s artifact=%s"),
		GetOwner() ? *GetOwner()->GetName() : TEXT("<none>"), *Artifact->GetPathName());
	return true;
}

bool UUERLPolicyComponent::GetRobotTransform(FTransform& OutTransform) const
{
	FString Error;
	if (!Controller.GetRobotTransform(OutTransform, Error))
	{
		OutTransform = FTransform::Identity;
		return false;
	}
	return true;
}

void UUERLPolicyComponent::StopPolicy()
{
	if (!bRunning && !Controller.IsInitialized())
	{
		return;
	}
	++LifecycleGeneration;
	bRunning = false;
	bBootstrapPending = false;
	bFaulted = false;
	AccumulatedPhysicsSeconds = 0.0;
	AccumulatedGameSeconds = 0.0;
	SetComponentTickEnabled(false);
	Controller.Shutdown();
	LatchedCommands.Reset();
	CommandAges.Reset();
	StaleChannels.Reset();
	LastSolverFrame = INDEX_NONE;
	LastSolverTime = 0.0;
}

bool UUERLPolicyComponent::SetCommand(FName Channel, const TArray<float>& Values)
{
	for (const FUERLPolicyCommandChannelInfo& Required : GetRequiredCommandChannels())
	{
		if (Required.Name != Channel)
		{
			continue;
		}
		if (Values.Num() != Required.Width)
		{
			LastError = FString::Printf(
				TEXT("command channel '%s' width %d does not match required width %d"),
				*Channel.ToString(), Values.Num(), Required.Width);
			if (LastLoggedSetCommandError != LastError)
			{
				LastLoggedSetCommandError = LastError;
				UE_LOG(LogUERLPolicy, Warning, TEXT("[UERLPolicyComponent] SetCommand failed: %s"), *LastError);
			}
			return false;
		}
		for (const float Value : Values)
		{
			if (!FMath::IsFinite(Value))
			{
				LastError = FString::Printf(TEXT("command channel '%s' contains a non-finite value"), *Channel.ToString());
				return false;
			}
		}
		LatchedCommands.Add(Channel, Values);
		CommandAges.Add(Channel, 0.0);
		StaleChannels.Remove(Channel);
		LastError.Reset();
		LastLoggedSetCommandError.Reset();
		return true;
	}
	LastError = FString::Printf(TEXT("unknown command channel '%s'"), *Channel.ToString());
	if (LastLoggedSetCommandError != LastError)
	{
		LastLoggedSetCommandError = LastError;
		UE_LOG(LogUERLPolicy, Warning, TEXT("[UERLPolicyComponent] SetCommand failed: %s"), *LastError);
	}
	return false;
}

TArray<FUERLPolicyCommandChannelInfo> UUERLPolicyComponent::GetRequiredCommandChannels() const
{
	TArray<FUERLPolicyCommandChannelInfo> Result;
	for (const FUERLPolicyCommandChannel& Required : Controller.RequiredCommands())
	{
		FUERLPolicyCommandChannelInfo& Info = Result.AddDefaulted_GetRef();
		Info.Name = Required.Name;
		Info.Width = Required.Width;
	}
	if (Result.Num() == 0 && Artifact)
	{
		const FUERLPolicyArtifactSummary& Summary = Artifact->Summary;
		for (int32 Index = 0; Index < Summary.CommandChannels.Num(); ++Index)
		{
			FUERLPolicyCommandChannelInfo& Info = Result.AddDefaulted_GetRef();
			Info.Name = Summary.CommandChannels[Index];
			Info.Width = Summary.CommandWidths[Index];
		}
	}
	return Result;
}

bool UUERLPolicyComponent::SoftReset()
{
	if (!Controller.IsInitialized())
	{
		LastError = TEXT("policy component is not running");
		return false;
	}
	Controller.Reset();
	RearmPolicyLoop();
	LastError.Reset();
	return true;
}

bool UUERLPolicyComponent::ResetToReferencePose()
{
	if (!Controller.IsInitialized())
	{
		LastError = TEXT("policy component is not running");
		return false;
	}
	FString Error;
	if (!Controller.ResetToReferencePose(Error))
	{
		Fault(Error);
		return false;
	}
	RearmPolicyLoop();
	LastError.Reset();
	return true;
}

void UUERLPolicyComponent::TickComponent(
	float DeltaTime,
	ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (bAutoStart && !bRunning && !bFaulted)
	{
		if (!StartPolicy())
		{
			SetComponentTickEnabled(false);
			return;
		}
	}
	if (!bRunning || bFaulted || !Controller.IsInitialized())
	{
		return;
	}
	// Sync the controlled mesh only; a class-wide lookup could grab an
	// unrelated mesh on a multi-mesh owner.
	if (USkeletalMeshComponent* Mesh = Controller.GetControlledMeshComponent())
	{
		Mesh->SyncComponentToRBPhysics();
	}

	// Game time advances even when the solver is paused.  It ages commands and
	// is retained for the next real solver window, but never creates a control
	// frame by itself.
	const double GameDeltaSeconds = FMath::Max(0.0, static_cast<double>(DeltaTime));
	AccumulatedGameSeconds += GameDeltaSeconds;
	TArray<TPair<FName, double>> NewlyStaleChannels;
	for (TPair<FName, double>& Age : CommandAges)
	{
		Age.Value += GameDeltaSeconds;
		if (CommandStalenessSeconds > 0.0
			&& Age.Value >= CommandStalenessSeconds
			&& !StaleChannels.Contains(Age.Key))
		{
			StaleChannels.Add(Age.Key);
			NewlyStaleChannels.Emplace(Age.Key, Age.Value);
		}
	}
	for (const TPair<FName, double>& Stale : NewlyStaleChannels)
	{
		const uint64 CallbackGeneration = LifecycleGeneration;
		OnCommandStale.Broadcast(Stale.Key, static_cast<float>(Stale.Value));
		if (CallbackGeneration != LifecycleGeneration
			|| !bRunning || bFaulted || !Controller.IsInitialized())
		{
			return;
		}
	}

	FString Error;
	FUERLSolverClockSnapshot Clock;
	if (!Controller.ReadCompletedSolverClock(Clock, Error))
	{
		Fault(Error);
		return;
	}
	if (LastSolverFrame != INDEX_NONE && Clock.Frame <= LastSolverFrame)
	{
		return;
	}
	const int32 PreviousFrame = LastSolverFrame;
	const double PhysicsSeconds = PreviousFrame == INDEX_NONE
		? Clock.LastDt
		: Clock.SolverTime - LastSolverTime;
	LastSolverFrame = Clock.Frame;
	LastSolverTime = Clock.SolverTime;
	if (!FMath::IsFinite(PhysicsSeconds) || PhysicsSeconds <= 0.0)
	{
		return;
	}
	if (!FMath::IsFinite(Clock.LastDt) || Clock.LastDt <= 0.0)
	{
		Fault(TEXT("completed solver clock has a non-positive last solver-step dt"));
		return;
	}
	AccumulatedPhysicsSeconds += PhysicsSeconds;

	if (!Artifact)
	{
		Fault(TEXT("policy component lost its Artifact while running"));
		return;
	}
	FUERLPolicyArtifact Parsed;
	if (!Artifact->LoadArtifact(Parsed, Error))
	{
		Fault(Error);
		return;
	}
	const double MinimumWindow = Parsed.Timing().DtMin();
	const double MaximumWindow = Parsed.Timing().DtMax();
	const double TimingTolerance = FMath::Max(1.0e-7, MaximumWindow * 1.0e-5);
	if (!bBootstrapPending && AccumulatedPhysicsSeconds < MinimumWindow - TimingTolerance)
	{
		return;
	}

	const double ObservationSeconds = bBootstrapPending
		? MinimumWindow
		: FMath::Clamp(AccumulatedPhysicsSeconds, MinimumWindow, MaximumWindow);
	const bool bControlOverrun = !bBootstrapPending
		&& (AccumulatedPhysicsSeconds > MaximumWindow + TimingTolerance
			|| AccumulatedGameSeconds > MaximumWindow + TimingTolerance);
	if (bControlOverrun)
	{
		const uint64 CallbackGeneration = LifecycleGeneration;
		OnControlStepOverrun.Broadcast(
			static_cast<float>(AccumulatedGameSeconds),
			static_cast<float>(AccumulatedPhysicsSeconds),
			static_cast<float>(ObservationSeconds));
		if (CallbackGeneration != LifecycleGeneration
			|| !bRunning || bFaulted || !Controller.IsInitialized())
		{
			return;
		}
	}

	FUERLPolicyCommands Commands;
	for (const TPair<FName, TArray<float>>& Pair : LatchedCommands)
	{
		Commands.Set(Pair.Key, Pair.Value);
	}
	FUERLControlTiming ControlTiming;
	ControlTiming.ObservationDtSeconds = ObservationSeconds;
	ControlTiming.LastSolverStepSeconds = Clock.LastDt;
	const uint64 StepGeneration = LifecycleGeneration;
	if (!Controller.Step(Commands, ControlTiming, Error))
	{
		Fault(Error);
		return;
	}
	if (bBootstrapPending)
	{
		UE_LOG(LogUERLPolicy, Display, TEXT("[UERLPolicyComponent] first control step frame=%d observation_dt=%.6f solver_dt=%.6f"),
			Clock.Frame, ObservationSeconds, Clock.LastDt);
	}
	if (StepGeneration != LifecycleGeneration || !bRunning || bFaulted)
	{
		return;
	}
	bBootstrapPending = false;
	AccumulatedPhysicsSeconds = 0.0;
	AccumulatedGameSeconds = 0.0;
}

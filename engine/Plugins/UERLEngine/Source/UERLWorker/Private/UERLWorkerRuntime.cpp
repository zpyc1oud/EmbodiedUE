#include "UERLWorkerRuntime.h"

#include "UERLRegistry.h"
#include "UERLFrameGate.h"
#include "UERLStepPipeline.h"
#include "UERLWorkerLog.h"

#include "Engine/World.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreMisc.h"
#include "Misc/Parse.h"
#include "UObject/Package.h"

#include <limits>

FUERLWorkerRuntime& FUERLWorkerRuntime::Get()
{
	static FUERLWorkerRuntime Instance;
	return Instance;
}

FUERLWorkerRuntime::~FUERLWorkerRuntime()
{
	Deactivate();
}

bool FUERLWorkerRuntime::Activate(const FUERLWorkerLaunchConfig& Config, FSessionEnded OnSessionEnded)
{
	if (bActive)
	{
		UE_LOG(LogUERLWorker, Warning, TEXT("[FLOW] Worker Runtime is already active"));
		return true;
	}
	if (!Config.IsValid())
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] invalid Worker launch configuration"));
		return false;
	}

	LaunchConfig = Config;
	{
		FScopeLock Guard(&SessionEndMutex);
		SessionEndedCallback = MoveTemp(OnSessionEnded);
		bSessionEndNotified = false;
	}
	ActiveProjection = FUERLWorkerProjection();
	ActiveProjection.PhysicsDt = UERLWorkerDefaults::PhysicsDt;
	ActiveProjection.DecimationMin = UERLWorkerDefaults::Decimation;
	ActiveProjection.DecimationMax = UERLWorkerDefaults::Decimation;
	Phase.Store(EUERLWorkerPhase::Booting);
	bBridgeDisconnected.Store(false);
	bFatalResponsePending.Store(false);
	bRequestPending.Store(false);
	RemainingFrames = 0;
	ActiveStepDecimation = UERLWorkerDefaults::Decimation;
	RequestEvent = FPlatformProcess::GetSynchEventFromPool(false);
	DoneEvent = FPlatformProcess::GetSynchEventFromPool(true);
	if (!RequestEvent || !DoneEvent)
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] failed to allocate Worker synchronization events"));
		Deactivate();
		return false;
	}

	FString Error;
	bActive = true;
	FUERLBridgeCapabilities Capabilities;
	Capabilities.ActivePresentationMode = LaunchConfig.PresentationMode == EUERLPresentationMode::None
		? TEXT("none")
		: LaunchConfig.PresentationMode == EUERLPresentationMode::Viewport
			? TEXT("viewport")
			: TEXT("gameplay");
	if (!BridgeServer.Start(LaunchConfig.Bridge, Capabilities, *this, Error))
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] Bridge activation failed: %s"), *Error);
		Deactivate();
		return false;
	}

	UE_LOG(LogUERLWorker, Display, TEXT("[FLOW] Worker Runtime activated bootstrap_port=%d; awaiting wire config"),
		LaunchConfig.Bridge.Port);
	return true;
}

void FUERLWorkerRuntime::Deactivate()
{
	if (!bActive && !RequestEvent && !DoneEvent)
	{
		return;
	}

	if (Pool) { Pool->EndControlWindow(); }
	Phase.Store(EUERLWorkerPhase::ShuttingDown);
	bBridgeDisconnected.Store(true);
	CloseFrameGate();
	if (RequestEvent) { RequestEvent->Trigger(); }
	if (DoneEvent)
	{
		FScopeLock Guard(&RequestMutex);
		if (bRequestPending.Load())
		{
			PendingRequest.Fail(UERLBridgeError::ShuttingDown, TEXT("Worker is shutting down"));
			bRequestPending.Store(false);
		}
		DoneEvent->Trigger();
	}

	DestroyTrainingResources();
	BridgeServer.Stop();
	FrameGate.Reset();
	StepPipeline.Reset();
	{
		FScopeLock Guard(&SessionEndMutex);
		SessionEndedCallback = nullptr;
	}

	if (RequestEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(RequestEvent);
		RequestEvent = nullptr;
	}
	if (DoneEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
		DoneEvent = nullptr;
	}
	bActive = false;
}

bool FUERLWorkerRuntime::BindPipeline(UUERLStepPipeline* Pipeline)
{
	if (!Pipeline)
	{
		return false;
	}
	if (StepPipeline.IsValid() && StepPipeline.Get() != Pipeline)
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] a Worker World is already bound; rejecting a second World"));
		return false;
	}
	StepPipeline = Pipeline;
	Phase.Store(EUERLWorkerPhase::WaitingInitialize);
	return true;
}

void FUERLWorkerRuntime::UnbindPipeline(UUERLStepPipeline* Pipeline)
{
	if (StepPipeline.Get() == Pipeline)
	{
		StepPipeline.Reset();
		if (bActive && Phase.Load() != EUERLWorkerPhase::ShuttingDown)
		{
			Phase.Store(EUERLWorkerPhase::Failed);
		}
	}
}

void FUERLWorkerRuntime::SubmitAndWait(FUERLBridgeRequest& InOutRequest)
{
	checkf(!IsInGameThread(), TEXT("Bridge requests must enter the Worker from its I/O thread"));
	if (!RequestEvent || !DoneEvent || !bActive)
	{
		InOutRequest.Fail(UERLBridgeError::ShuttingDown, TEXT("Worker Runtime is inactive"));
		return;
	}
	if (InOutRequest.Type == EUERLBridgeRequestType::Step)
	{
		InOutRequest.StepTiming.SubmittedAtSeconds = FPlatformTime::Seconds();
	}
	else if (InOutRequest.Type == EUERLBridgeRequestType::Reset)
	{
		InOutRequest.ResetTiming.SubmittedAtSeconds = FPlatformTime::Seconds();
	}

	{
		FScopeLock Guard(&RequestMutex);
		if (bRequestPending.Load())
		{
			InOutRequest.Fail(UERLBridgeError::AlreadyInflight, TEXT("a Worker request is already pending"));
			return;
		}
		PendingRequest = InOutRequest;
		PendingRequest.bOk = false;
		PendingRequest.ErrorCode = UERLBridgeError::WorkerFatal;
		PendingRequest.ErrorMessage.Reset();
		DoneEvent->Reset();
		bRequestPending.Store(true);
	}
	RequestEvent->Trigger();

	bool bCompleted = false;
	const double Deadline = FPlatformTime::Seconds() + FMath::Max(1u, InOutRequest.TimeoutMs) / 1000.0;
	while (!(bCompleted = DoneEvent->Wait(FTimespan::FromMilliseconds(50))))
	{
		if (bBridgeDisconnected.Load() || IsEngineExitRequested() || FPlatformTime::Seconds() >= Deadline)
		{
			break;
		}
	}

	FScopeLock Guard(&RequestMutex);
	if (bCompleted || !bRequestPending.Load())
	{
		InOutRequest = PendingRequest;
	}
	else
	{
		InOutRequest.Fail(UERLBridgeError::TransportFailure, TEXT("Worker request deadline expired or session stopped"));
		NotifyBridgeDisconnected();
	}
}

void FUERLWorkerRuntime::NotifyBridgeDisconnected()
{
	bBridgeDisconnected.Store(true);
	if (RequestEvent) { RequestEvent->Trigger(); }
}

bool FUERLWorkerRuntime::BindFrameGate(UUERLFrameGate* InFrameGate)
{
	if (!InFrameGate || (FrameGate.IsValid() && FrameGate.Get() != InFrameGate))
	{
		return false;
	}
	FrameGate = InFrameGate;
	return true;
}

void FUERLWorkerRuntime::UnbindFrameGate(UUERLFrameGate* InFrameGate)
{
	if (FrameGate.Get() == InFrameGate)
	{
		FrameGate.Reset();
	}
}

void FUERLWorkerRuntime::NotifyBridgeShutdownComplete()
{
	NotifySessionEnded(EUERLSessionEndReason::GracefulShutdown);
}

bool FUERLWorkerRuntime::TryTakeRequest(FUERLBridgeRequest& OutRequest)
{
	if (!bRequestPending.Load())
	{
		return false;
	}
	FScopeLock Guard(&RequestMutex);
	if (!bRequestPending.Load())
	{
		return false;
	}
	OutRequest = PendingRequest;
	return true;
}

void FUERLWorkerRuntime::WaitForRequest(uint32 TimeoutMs)
{
	if (RequestEvent) { RequestEvent->Wait(TimeoutMs); }
}

void FUERLWorkerRuntime::CompleteRequest(const FUERLBridgeRequest& Response)
{
	{
		FScopeLock Guard(&RequestMutex);
		PendingRequest = Response;
		bRequestPending.Store(false);
	}
	if (DoneEvent) { DoneEvent->Trigger(); }
}

void FUERLWorkerRuntime::FailPendingRequest(int32 ErrorCode, const FString& Message)
{
	FUERLBridgeRequest Request;
	if (TryTakeRequest(Request))
	{
		Request.Fail(ErrorCode, Message);
		CompleteRequest(Request);
	}
}

void FUERLWorkerRuntime::StopForBridgeDisconnect()
{
	const bool bWorkerFatal = Phase.Load() == EUERLWorkerPhase::Failed;
	const int32 StoppedFrames = RemainingFrames;
	FailPendingRequest(UERLBridgeError::ShuttingDown, TEXT("Bridge disconnected or request deadline expired"));
	CloseFrameGate();
	DestroyTrainingResources();
	if (Phase.Load() != EUERLWorkerPhase::ShuttingDown)
	{
		Phase.Store(EUERLWorkerPhase::Failed);
	}
	UE_LOG(LogUERLWorker, Display,
		TEXT("[VERIFY] Bridge failure stopped Worker with remaining_frames=%d"), StoppedFrames);
	NotifySessionEnded(bWorkerFatal
		? EUERLSessionEndReason::WorkerFatal
		: EUERLSessionEndReason::BridgeDisconnected);
}

bool FUERLWorkerRuntime::StopForWorkerFailure()
{
	const bool bAwaitTransport = bFatalResponsePending.Load() || bRequestPending.Load();
	if (bRequestPending.Load())
	{
		bFatalResponsePending.Store(true);
	}
	FailPendingRequest(UERLBridgeError::WorkerFatal, TEXT("Worker is in Failed state"));
	CloseFrameGate();
	DestroyTrainingResources();
	if (!bAwaitTransport)
	{
		NotifySessionEnded(EUERLSessionEndReason::WorkerFatal);
	}
	return bAwaitTransport;
}

void FUERLWorkerRuntime::ProcessRequestGameThread(
	FUERLBridgeRequest& Request,
	bool& bOutReleaseFrame,
	bool& bOutExit)
{
	check(IsInGameThread());
	bOutReleaseFrame = false;
	bOutExit = false;
	bool bExitAfterCompletion = false;
	if (Request.Type == EUERLBridgeRequestType::Step)
	{
		Request.StepTiming.QueueWaitSeconds =
			FPlatformTime::Seconds() - Request.StepTiming.SubmittedAtSeconds;
	}
	else if (Request.Type == EUERLBridgeRequestType::Reset)
	{
		Request.ResetTiming.QueueWaitSeconds =
			FPlatformTime::Seconds() - Request.ResetTiming.SubmittedAtSeconds;
	}

	switch (Request.Type)
	{
	case EUERLBridgeRequestType::PrepareInitialize:
		PrepareInitialize(Request);
		break;
	case EUERLBridgeRequestType::CommitInitialize:
		CommitInitialize(Request);
		bOutReleaseFrame = Request.bOk
			&& Phase.Load() == EUERLWorkerPhase::StabilizingInitialize;
		break;
	case EUERLBridgeRequestType::AbortInitialize:
		AbortInitialize(Request);
		break;
	case EUERLBridgeRequestType::Ready:
		ExecuteReady(Request);
		break;
	case EUERLBridgeRequestType::Step:
		BeginStep(Request);
		bOutReleaseFrame = Request.bOk;
		break;
	case EUERLBridgeRequestType::Event:
		ExecuteEvent(Request);
		break;
	case EUERLBridgeRequestType::Reset:
		ExecuteReset(Request);
		break;
	case EUERLBridgeRequestType::Shutdown:
		ExecuteShutdown(Request);
		bExitAfterCompletion = true;
		break;
	default:
		Request.Fail(UERLBridgeError::ProtocolViolation, TEXT("unknown Worker request"));
		break;
	}

	if (!bOutReleaseFrame)
	{
		CompleteRequest(Request);
	}
	bOutExit = bExitAfterCompletion;
}

void FUERLWorkerRuntime::PrepareInitialize(FUERLBridgeRequest& Request)
{
	if (!RequirePhase(EUERLWorkerPhase::WaitingInitialize, TEXT("PrepareInitialize"), Request))
	{
		return;
	}
	FString Error;
	if (!Request.Projection.IsWorkerConfigValid())
	{
		FailWorker(
			TEXT("Initialize worker_config is incomplete or invalid"),
			&Request,
			UERLBridgeError::ConfigRejected);
		return;
	}
	if (!Request.Projection.EnvironmentConfig.ResetDistributions.IsEmpty()
		|| !Request.Projection.RobotConfig.ResetDistributions.IsEmpty())
	{
		FailWorker(
			TEXT("UE provider reset distributions are unsupported; Python must supply sampled reset values"),
			&Request,
			UERLBridgeError::ConfigRejected);
		return;
	}
	UUERLStepPipeline* Pipeline = StepPipeline.Get();
	if (!Pipeline || !Pipeline->IsReady())
	{
		FailWorker(TEXT("training World is not ready"), &Request);
		return;
	}
	UWorld* World = Pipeline->GetBoundWorld();
	check(World);
	const FString LoadedWorldMap = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	if (LoadedWorldMap != Request.Projection.WorldMap)
	{
		FailWorker(
			FString::Printf(
				TEXT("loaded World '%s' does not match worker_config.world_map '%s'"),
				*LoadedWorldMap,
				*Request.Projection.WorldMap),
			&Request,
			UERLBridgeError::ConfigRejected);
		return;
	}
	if (!Pipeline->DidStartupGatePass() || !Pipeline->ValidateRequestedPhysics(Request.Projection.PhysicsDt, Error))
	{
		FailWorker(
			Error.IsEmpty() ? Pipeline->GetGateFailure() : Error,
			&Request,
			UERLBridgeError::ConfigRejected);
		return;
	}

	PreparedEnvironmentFactory = FUERLEnvironmentRegistry::Get().Resolve(Request.Projection.EnvironmentId, Error);
	if (!PreparedEnvironmentFactory)
	{
		FailWorker(Error, &Request, UERLBridgeError::ConfigRejected);
		return;
	}
	PreparedRobotFactory = FUERLRobotRegistry::Get().Resolve(Request.Projection.RobotId, Error);
	if (!PreparedRobotFactory)
	{
		ClearPreparedState();
		FailWorker(Error, &Request, UERLBridgeError::ConfigRejected);
		return;
	}
	if (!PreparedEnvironmentFactory->ValidateConfig(Request.Projection.EnvironmentConfig, EffectiveEnvironmentConfig, Error)
		|| !PreparedRobotFactory->ValidateConfig(Request.Projection.RobotConfig, EffectiveRobotConfig, Error))
	{
		ClearPreparedState();
		FailWorker(Error, &Request, UERLBridgeError::ConfigRejected);
		return;
	}

	AvailableActionFields = PreparedRobotFactory->Describe().ActionFields;
	AvailableStateFields = PreparedEnvironmentFactory->Describe().StateFields;
	AvailableStateFields.Append(PreparedRobotFactory->Describe().StateFields);
	Request.Projection.EnvironmentConfig = EffectiveEnvironmentConfig;
	Request.Projection.RobotConfig = EffectiveRobotConfig;
	Request.AvailableActionFields = AvailableActionFields;
	Request.AvailableStateFields = AvailableStateFields;
	Request.AvailableTopology = PreparedRobotFactory->Describe().Topology;
	ActiveProjection = Request.Projection;
	Request.Succeed();
	Phase.Store(EUERLWorkerPhase::Prepared);
}

void FUERLWorkerRuntime::CommitInitialize(FUERLBridgeRequest& Request)
{
	if (!RequirePhase(EUERLWorkerPhase::Prepared, TEXT("CommitInitialize"), Request))
	{
		return;
	}
	checkf(PreparedEnvironmentFactory && PreparedRobotFactory,
		TEXT("Prepared Worker is missing its provider factories"));
	FString Error;
	if (!Request.Projection.IsValid())
	{
		FailWorker(TEXT("CommitInitialize projection is incomplete or invalid"), &Request);
		return;
	}
	if (!PreparedEnvironmentFactory->ValidateConfig(Request.Projection.EnvironmentConfig, EffectiveEnvironmentConfig, Error)
		|| !PreparedRobotFactory->ValidateConfig(Request.Projection.RobotConfig, EffectiveRobotConfig, Error))
	{
		ClearPreparedState();
		FailWorker(Error, &Request, UERLBridgeError::ConfigRejected);
		return;
	}
	AvailableActionFields = PreparedRobotFactory->Describe().ActionFields;
	AvailableStateFields = PreparedEnvironmentFactory->Describe().StateFields;
	AvailableStateFields.Append(PreparedRobotFactory->Describe().StateFields);
	Request.Projection.EnvironmentConfig = EffectiveEnvironmentConfig;
	Request.Projection.RobotConfig = EffectiveRobotConfig;
	Request.AvailableActionFields = AvailableActionFields;
	Request.AvailableStateFields = AvailableStateFields;
	Request.AvailableTopology = PreparedRobotFactory->Describe().Topology;
	ActiveProjection = Request.Projection;
	if (!BatchBinding.Compile(Request.SelectedSchema, AvailableActionFields, AvailableStateFields, Error))
	{
		ClearPreparedState();
		FailWorker(Error, &Request);
		return;
	}
	if (!ValidateStateBatch(Request, Request.Projection, Error))
	{
		ClearPreparedState();
		FailWorker(Error, &Request);
		return;
	}
	UUERLStepPipeline* Pipeline = StepPipeline.Get();
	UWorld* World = Pipeline ? Pipeline->GetBoundWorld() : nullptr;
	if (!World)
	{
		FailWorker(TEXT("training World disappeared during CommitInitialize"), &Request);
		return;
	}

	TArray<FUERLFieldDescriptor> SelectedStateFields;
	SelectedStateFields.Reserve(Request.SelectedSchema.StateFields.Num());
	for (const FUERLBatchFieldBinding& Binding : Request.SelectedSchema.StateFields)
	{
		SelectedStateFields.Add(Binding.Field);
	}
	Pool = MakeUnique<FUERLEnvironmentPool>();
	if (!Pool->Create(*World, Request.Projection.NumSlots,
		PreparedEnvironmentFactory.ToSharedRef(), EffectiveEnvironmentConfig,
		PreparedRobotFactory.ToSharedRef(), EffectiveRobotConfig, SelectedStateFields,
		Request.Projection.TerrainConfig, Error))
	{
		FailWorker(Error, &Request);
		return;
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (FParse::Param(FCommandLine::Get(), TEXT("uerltestinitializefailure")))
	{
		FailWorker(TEXT("test-only forced Initialize failure after Pool creation"), &Request);
		return;
	}
#endif

	if (!Pool->InitializeSlots(Request.TerrainLevels, Error))
	{
		FailWorker(Error, &Request);
		return;
	}

	Safety.Initialize(Request.Projection.NumSlots, Request.SelectedSchema.StateWidth);
	ActiveInitializeRequest = Request;
	ActiveInitializeRequest.Succeed();
	ClearPreparedState();
	Request.Succeed();
	Phase.Store(EUERLWorkerPhase::StabilizingInitialize);
	UE_LOG(LogUERLWorker, Display,
		TEXT("[FLOW] Worker spawned slots=%d -> StabilizingInitialize"), Pool->Num());
}

void FUERLWorkerRuntime::FinishInitializeStabilization()
{
	if (Phase.Load() != EUERLWorkerPhase::StabilizingInitialize || !Pool)
	{
		return;
	}
	FString Error;
	const TArray<int32>& AllSlots = Pool->AllSlotIds();
	FUERLNamedStateWriter Writer(BatchBinding, ActiveInitializeRequest.States);
	Writer.FillAll(std::numeric_limits<float>::quiet_NaN());
	if (!Pool->CollectState(AllSlots, Writer, Error))
	{
		FailWorker(Error, &ActiveInitializeRequest);
		CompleteRequest(ActiveInitializeRequest);
		return;
	}
	Pool->ValidateSlots(AllSlots, ProviderFaults);
	if (!Safety.AcceptInitial(
		ActiveInitializeRequest.States,
		ProviderFaults,
		ActiveInitializeRequest.Faults,
		Error))
	{
		FailWorker(Error, &ActiveInitializeRequest);
		CompleteRequest(ActiveInitializeRequest);
		return;
	}
	UUERLStepPipeline* Pipeline = StepPipeline.Get();
	if (!Pipeline || !Pipeline->CaptureSolverBaseline(Error))
	{
		FailWorker(
			Error.IsEmpty() ? TEXT("training World disappeared during Initialize stabilization") : Error,
			&ActiveInitializeRequest);
		CompleteRequest(ActiveInitializeRequest);
		return;
	}
	ActiveInitializeRequest.EpisodeIndices = Pool->EpisodeIndices();
	ActiveInitializeRequest.Succeed();
	Phase.Store(EUERLWorkerPhase::WaitingReady);
	CompleteRequest(ActiveInitializeRequest);
	ActiveInitializeRequest = FUERLBridgeRequest();
	UE_LOG(LogUERLWorker, Display,
		TEXT("[FLOW] Worker physics stabilized -> WaitingReady"));
}

void FUERLWorkerRuntime::ExecuteReady(FUERLBridgeRequest& Request)
{
	if (!RequirePhase(EUERLWorkerPhase::WaitingReady, TEXT("Ready"), Request))
	{
		return;
	}
	checkf(Pool, TEXT("WaitingReady Worker is missing its Environment Pool"));
	Phase.Store(EUERLWorkerPhase::Idle);
	Request.Succeed();
	UE_LOG(LogUERLWorker, Display, TEXT("[FLOW] Ready manifest acknowledged -> Idle"));
}

void FUERLWorkerRuntime::AbortInitialize(FUERLBridgeRequest& Request)
{
	if (!RequirePhase(EUERLWorkerPhase::Prepared, TEXT("AbortInitialize"), Request))
	{
		return;
	}
	ClearPreparedState();
	Phase.Store(EUERLWorkerPhase::WaitingInitialize);
	Request.Succeed();
}

void FUERLWorkerRuntime::BeginStep(FUERLBridgeRequest& Request)
{
	if (!RequirePhase(EUERLWorkerPhase::Idle, TEXT("Step"), Request))
	{
		return;
	}
	checkf(Pool && BatchBinding.IsCompiled(), TEXT("Idle Worker is missing its compiled execution resources"));
	const FUERLWorkerProjection& Projection = ActiveProjection;
	FString Error;
	if (Request.StepDecimation < Projection.DecimationMin || Request.StepDecimation > Projection.DecimationMax)
	{
		FailWorker(TEXT("Step decimation is outside the negotiated range"), &Request);
		return;
	}
	if (!Request.Actions.IsValid()
		|| Request.Actions.NumRows != Projection.NumSlots || Request.Actions.NumColumns != Projection.ActionWidth
		|| !ValidateStateBatch(Request, Projection, Error))
	{
		FailWorker(Error.IsEmpty() ? TEXT("Step Action batch does not match selected Schema") : Error, &Request);
		return;
	}
	for (int32 Row = 0; Row < Request.Actions.NumRows; ++Row)
	{
		for (int32 Column = 0; Column < Request.Actions.NumColumns; ++Column)
		{
			if (!FMath::IsFinite(Request.Actions.At(Row, Column)))
			{
				FailWorker(TEXT("Step contains a non-finite Physical Command"), &Request);
				return;
			}
		}
	}

	ActiveStepRequest = Request;
	ActiveStepRequest.Succeed();
	ActivePhysicsFrameStartSeconds = 0.0;
	ActiveStepDecimation = Request.StepDecimation;
	RemainingFrames = ActiveStepDecimation;
	Phase.Store(EUERLWorkerPhase::ActiveStep);
	Pool->BeginControlWindow();
	Request.Succeed();
}

bool FUERLWorkerRuntime::ApplyActiveCommands()
{
	if (Phase.Load() != EUERLWorkerPhase::ActiveStep || !Pool)
	{
		return false;
	}
	FUERLNamedActionReader Reader(BatchBinding, ActiveStepRequest.Actions);
	FString Error;
	const double StartSeconds = FPlatformTime::Seconds();
	const bool bApplied = Pool->ApplyCommands(Reader, Error);
	ActiveStepRequest.StepTiming.ActionApplySeconds += FPlatformTime::Seconds() - StartSeconds;
	++ActiveStepRequest.StepTiming.ActionApplyCount;
	if (!bApplied)
	{
		FailWorker(Error, &ActiveStepRequest);
		CompleteRequest(ActiveStepRequest);
		return false;
	}
	return true;
}

bool FUERLWorkerRuntime::ShouldApplyActiveCommands() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (FParse::Param(FCommandLine::Get(), TEXT("uerltestapplyactionseveryframe")))
	{
		return true;
	}
#endif
	return RemainingFrames == GetActiveStepDecimation() || Pool->RequiresCommandsEveryPhysicsFrame();
}

void FUERLWorkerRuntime::AdvanceEnvironmentPhysicsFrame()
{
	if (Pool && Phase.Load() == EUERLWorkerPhase::ActiveStep)
	{
		Pool->AdvancePhysicsFrame(ActiveProjection.PhysicsDt);
	}
}

void FUERLWorkerRuntime::BeginPhysicsFrameTiming()
{
	ActivePhysicsFrameStartSeconds = FPlatformTime::Seconds();
}

void FUERLWorkerRuntime::EndPhysicsFrameTiming()
{
	ActiveStepRequest.StepTiming.PhysicsFrameSeconds +=
		FPlatformTime::Seconds() - ActivePhysicsFrameStartSeconds;
	ActivePhysicsFrameStartSeconds = 0.0;
}

int32 FUERLWorkerRuntime::CompletePhysicsFrame()
{
	if (Phase.Load() == EUERLWorkerPhase::ActiveStep && RemainingFrames > 0)
	{
		--RemainingFrames;
#if WITH_DEV_AUTOMATION_TESTS
		int32 TestDelayMs = 0;
		if (RemainingFrames == GetActiveStepDecimation() - 1
			&& FParse::Value(FCommandLine::Get(), TEXT("uerltestpostphysicsdelayms="), TestDelayMs)
			&& TestDelayMs > 0 && TestDelayMs <= 10000)
		{
			UE_LOG(LogUERLWorker, Display,
				TEXT("[VERIFY] test-only post-physics delay=%dms remaining_frames=%d"), TestDelayMs, RemainingFrames);
			FPlatformProcess::SleepNoStats(TestDelayMs / 1000.0f);
		}
#endif
	}
	return RemainingFrames;
}

void FUERLWorkerRuntime::FinishActiveStep()
{
	if (Phase.Load() != EUERLWorkerPhase::ActiveStep || !Pool)
	{
		return;
	}
	Pool->EndControlWindow();
	const double StateStartSeconds = FPlatformTime::Seconds();
	const double ContactStartSeconds = FPlatformTime::Seconds();
	Pool->SamplePhysicsContacts(ActiveProjection.PhysicsDt);
	ActiveStepRequest.StepTiming.ContactSampleSeconds +=
		FPlatformTime::Seconds() - ContactStartSeconds;
	const TArray<int32>& AllSlots = Pool->AllSlotIds();
	FUERLNamedStateWriter Writer(BatchBinding, ActiveStepRequest.States);
	Writer.FillAll(std::numeric_limits<float>::quiet_NaN());
	FString Error;
	if (!Pool->CollectState(AllSlots, Writer, Error))
	{
		ActiveStepRequest.StepTiming.StateCollectSeconds = FPlatformTime::Seconds() - StateStartSeconds;
		FailWorker(Error, &ActiveStepRequest);
		CompleteRequest(ActiveStepRequest);
		return;
	}
	Pool->ValidateSlots(AllSlots, ProviderFaults);
	if (!Safety.SanitizeTransition(ActiveStepRequest.States, ProviderFaults, ActiveStepRequest.Faults, Error))
	{
		ActiveStepRequest.StepTiming.StateCollectSeconds = FPlatformTime::Seconds() - StateStartSeconds;
		FailWorker(Error, &ActiveStepRequest);
		CompleteRequest(ActiveStepRequest);
		return;
	}
	ActiveStepRequest.StepTiming.StateCollectSeconds = FPlatformTime::Seconds() - StateStartSeconds;
	ActiveStepRequest.Succeed();
	CompleteRequest(ActiveStepRequest);
	RemainingFrames = 0;
	Phase.Store(EUERLWorkerPhase::Idle);
}

void FUERLWorkerRuntime::FailActiveStep(const FString& Message)
{
	if (Phase.Load() != EUERLWorkerPhase::ActiveStep)
	{
		return;
	}
	FailWorker(Message, &ActiveStepRequest);
	CompleteRequest(ActiveStepRequest);
}

void FUERLWorkerRuntime::ExecuteEvent(FUERLBridgeRequest& Request)
{
	if (!RequirePhase(EUERLWorkerPhase::Idle, TEXT("Event"), Request))
	{
		return;
	}
	checkf(Pool, TEXT("Idle Worker is missing its Environment Pool"));
	FString Error;
	if (!Pool->ApplyEvent(Request.Event, Error))
	{
		FailWorker(Error, &Request, UERLBridgeError::ConfigRejected);
		return;
	}
	Request.Succeed();
}

void FUERLWorkerRuntime::ExecuteReset(FUERLBridgeRequest& Request)
{
	if (!RequirePhase(EUERLWorkerPhase::Idle, TEXT("Reset"), Request))
	{
		return;
	}
	checkf(Pool, TEXT("Idle Worker is missing its Environment Pool"));
#if WITH_DEV_AUTOMATION_TESTS
	if (FParse::Param(FCommandLine::Get(), TEXT("uerltestresetfailure")))
	{
		FailWorker(TEXT("test-only forced Reset failure"), &Request);
		return;
	}
#endif
	FString Error;
	if (!ValidateStateBatch(Request, ActiveProjection, Error))
	{
		FailWorker(Error, &Request);
		return;
	}
	const double ResetStartSeconds = FPlatformTime::Seconds();
	if (!Pool->ResetSlots(Request.ResetSlots, Request.TerrainLevels, Request.ResetValues, Error))
	{
		FailWorker(Error, &Request);
		return;
	}
	Request.ResetTiming.ResetSlotsSeconds = FPlatformTime::Seconds() - ResetStartSeconds;
	const double StateStartSeconds = FPlatformTime::Seconds();
	FUERLNamedStateWriter Writer(BatchBinding, Request.States);
	Writer.FillRows(Request.ResetSlots, std::numeric_limits<float>::quiet_NaN());
	if (!Pool->CollectState(Request.ResetSlots, Writer, Error))
	{
		Request.ResetTiming.StateCollectSeconds = FPlatformTime::Seconds() - StateStartSeconds;
		FailWorker(Error, &Request);
		return;
	}
	Request.ResetTiming.StateCollectSeconds = FPlatformTime::Seconds() - StateStartSeconds;
	const double ValidateStartSeconds = FPlatformTime::Seconds();
	Pool->ValidateSlots(Request.ResetSlots, ProviderFaults);
	Request.ResetTiming.ValidateSlotsSeconds = FPlatformTime::Seconds() - ValidateStartSeconds;
	const double SafetyStartSeconds = FPlatformTime::Seconds();
	if (!Safety.AcceptReset(Request.States, Request.ResetSlots, ProviderFaults, Request.Faults, Error))
	{
		FailWorker(Error, &Request);
		return;
	}
	Request.ResetTiming.SafetySeconds = FPlatformTime::Seconds() - SafetyStartSeconds;
	const double EpisodeStartSeconds = FPlatformTime::Seconds();
	Request.EpisodeIndices = Pool->EpisodeIndices();
	Request.ResetTiming.EpisodeCopySeconds = FPlatformTime::Seconds() - EpisodeStartSeconds;
	Request.Succeed();
}

void FUERLWorkerRuntime::ExecuteShutdown(FUERLBridgeRequest& Request)
{
	if (Pool) { Pool->EndControlWindow(); }
	Phase.Store(EUERLWorkerPhase::ShuttingDown);
	CloseFrameGate();
	if (Pool) { Request.EpisodeIndices = Pool->EpisodeIndices(); }
	DestroyTrainingResources();
	Request.Succeed();
}

bool FUERLWorkerRuntime::RequirePhase(
	EUERLWorkerPhase Expected,
	const TCHAR* Operation,
	FUERLBridgeRequest& Request)
{
	const EUERLWorkerPhase Actual = Phase.Load();
	if (Actual == Expected)
	{
		return true;
	}
	FailWorker(FString::Printf(
		TEXT("%s is invalid in Worker phase %d; expected phase %d"),
		Operation, static_cast<int32>(Actual), static_cast<int32>(Expected)), &Request);
	return false;
}

bool FUERLWorkerRuntime::ValidateStateBatch(
	const FUERLBridgeRequest& Request,
	const FUERLWorkerProjection& Projection,
	FString& OutError) const
{
	if (!Request.States.IsValid() || !Request.Faults.IsValid()
		|| Request.States.NumRows != Projection.NumSlots
		|| Request.States.NumColumns != Projection.StateWidth
		|| Request.Faults.NumRows != Projection.NumSlots)
	{
		OutError = TEXT("Worker State/Fault batch does not match the selected Schema");
		return false;
	}
	return true;
}

void FUERLWorkerRuntime::FailWorker(
	const FString& Message,
	FUERLBridgeRequest* Request,
	int32 ErrorCode)
{
	if (Pool) { Pool->EndControlWindow(); }
	UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] Worker Session-fatal: %s"), *Message);
	Phase.Store(EUERLWorkerPhase::Failed);
	RemainingFrames = 0;
	if (Request)
	{
		bFatalResponsePending.Store(true);
		Request->Fail(ErrorCode, Message);
	}
}

void FUERLWorkerRuntime::ClearPreparedState()
{
	PreparedEnvironmentFactory.Reset();
	PreparedRobotFactory.Reset();
	EffectiveEnvironmentConfig = FUERLProviderConfig();
	EffectiveRobotConfig = FUERLProviderConfig();
	AvailableActionFields.Reset();
	AvailableStateFields.Reset();
}

void FUERLWorkerRuntime::CloseFrameGate()
{
	if (UUERLFrameGate* Gate = FrameGate.Get())
	{
		Gate->Close();
	}
}

void FUERLWorkerRuntime::DestroyTrainingResources()
{
	const int32 DestroyedSlots = Pool ? Pool->Num() : 0;
	const int32 StoppedFrames = RemainingFrames;
	if (UUERLStepPipeline* Pipeline = StepPipeline.Get())
	{
		Pipeline->DeactivateForWorker();
	}
	RemainingFrames = 0;
	ActiveStepDecimation = UERLWorkerDefaults::Decimation;
	ActiveInitializeRequest = FUERLBridgeRequest();
	ActiveStepRequest = FUERLBridgeRequest();
	if (Pool)
	{
		Pool->Destroy();
		Pool.Reset();
	}
	Safety.Reset();
	ProviderFaults.Reset();
	BatchBinding.Reset();
	ClearPreparedState();
	UE_LOG(LogUERLWorker, Display,
		TEXT("[VERIFY] Worker training resources destroyed slots=%d remaining_frames=%d"),
		DestroyedSlots, StoppedFrames);
}

void FUERLWorkerRuntime::NotifySessionEnded(EUERLSessionEndReason Reason)
{
	FSessionEnded Callback;
	{
		FScopeLock Guard(&SessionEndMutex);
		if (bSessionEndNotified)
		{
			return;
		}
		bSessionEndNotified = true;
		Callback = SessionEndedCallback;
	}
	if (Callback)
	{
		Callback(Reason);
	}
}

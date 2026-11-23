#include "UERLWorkerRuntime.h"
#include "UERLHostInputs.h"

#include "UERLRegistry.h"
#include "UERLFrameGate.h"
#include "UERLStepPipeline.h"
#include "UERLWorkerLog.h"

#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "Tests/UERLPhysicalFeedbackCapture.h"

#include <limits>

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
#if WITH_DEV_AUTOMATION_TESTS
	if (!CaptureUERLPhysicalFeedback(StepPipeline.IsValid() ? StepPipeline->GetBoundWorld() : nullptr,
		ActiveStepRequest, BatchBinding, Pool->PhysicalTestSlot(0), Error))
	{
		FailWorker(Error, &ActiveStepRequest);
		CompleteRequest(ActiveStepRequest);
		return;
	}
#endif
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
#if WITH_DEV_AUTOMATION_TESTS
	if (!CaptureUERLPhysicalFeedback(StepPipeline.IsValid() ? StepPipeline->GetBoundWorld() : nullptr,
		Request, BatchBinding, Pool->PhysicalTestSlot(0), Error))
	{
		FailWorker(Error, &Request);
		return;
	}
#endif
	Request.ResetTiming.SafetySeconds = FPlatformTime::Seconds() - SafetyStartSeconds;
	const double EpisodeStartSeconds = FPlatformTime::Seconds();
	Request.EpisodeIndices = Pool->EpisodeIndices();
	Request.ResetTiming.EpisodeCopySeconds = FPlatformTime::Seconds() - EpisodeStartSeconds;
	Request.Succeed();
}

void FUERLWorkerRuntime::ExecuteShutdown(FUERLBridgeRequest& Request)
{
	Phase.Store(EUERLWorkerPhase::ShuttingDown);
	CloseFrameGate();
	if (Pool) { Request.EpisodeIndices = Pool->EpisodeIndices(); }
	DestroyTrainingResources();
	Request.Succeed();
}

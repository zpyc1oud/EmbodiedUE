#include "UERLFrameGate.h"

#include "UERLWorkerLog.h"
#include "UERLWorkerRuntime.h"

#include "Engine/Engine.h"
#include "Misc/App.h"

bool UUERLFrameGate::Initialize(UEngine* InEngine)
{
	bClosed = false;
	SyncState = ECustomTimeStepSynchronizationState::Synchronized;
	UE_LOG(LogUERLWorker, Display, TEXT("[FLOW] request-driven Frame Gate installed"));
	return true;
}

void UUERLFrameGate::Shutdown(UEngine* InEngine)
{
	Close();
}

void UUERLFrameGate::Close()
{
	if (!bClosed)
	{
		bClosed = true;
		SyncState = ECustomTimeStepSynchronizationState::Closed;
		UE_LOG(LogUERLWorker, Display, TEXT("[FLOW] request-driven Frame Gate closed"));
	}
}

ECustomTimeStepSynchronizationState UUERLFrameGate::GetSynchronizationState() const
{
	return SyncState;
}

FString UUERLFrameGate::GetDisplayName() const
{
	return TEXT("UERL Frame Gate");
}

void UUERLFrameGate::AdvanceAppTimeFixed(double PhysicsDt)
{
	UEngineCustomTimeStep::UpdateApplicationLastTime();
	FApp::SetDeltaTime(PhysicsDt);
	FApp::SetCurrentTime(FApp::GetLastTime() + PhysicsDt);
}

bool UUERLFrameGate::UpdateTimeStep(UEngine* InEngine)
{
	if (bClosed)
	{
		return true;
	}
	FUERLWorkerRuntime& Runtime = FUERLWorkerRuntime::Get();
	if (!Runtime.IsActive())
	{
		return true;
	}

	const EUERLWorkerPhase Phase = Runtime.GetPhase();
	if (Phase == EUERLWorkerPhase::Booting)
	{
		return true;
	}
	if (Phase == EUERLWorkerPhase::ShuttingDown)
	{
		Close();
		return true;
	}
	if (Runtime.IsBridgeDisconnected())
	{
		Runtime.StopForBridgeDisconnect();
		return true;
	}
	if (Phase == EUERLWorkerPhase::Failed)
	{
		if (!Runtime.StopForWorkerFailure())
		{
			return true;
		}
	}
	if (Phase == EUERLWorkerPhase::ActiveStep && Runtime.GetRemainingFrames() > 0)
	{
		AdvanceAppTimeFixed(Runtime.GetPhysicsDt());
		return false;
	}
	if (Phase == EUERLWorkerPhase::StabilizingInitialize)
	{
		AdvanceAppTimeFixed(Runtime.GetPhysicsDt());
		return false;
	}

	for (;;)
	{
		if (Runtime.IsBridgeDisconnected())
		{
			Runtime.StopForBridgeDisconnect();
			return true;
		}
		if (Runtime.GetPhase() == EUERLWorkerPhase::Failed)
		{
			if (!Runtime.StopForWorkerFailure())
			{
				return true;
			}
			Runtime.WaitForRequest(20);
			continue;
		}

		FUERLBridgeRequest Request;
		if (Runtime.TryTakeRequest(Request))
		{
			bool bReleaseFrame = false;
			bool bExit = false;
			Runtime.ProcessRequestGameThread(Request, bReleaseFrame, bExit);
			if (bExit)
			{
				return true;
			}
			if (bReleaseFrame)
			{
				AdvanceAppTimeFixed(Runtime.GetPhysicsDt());
				return false;
			}
			continue;
		}

		Runtime.WaitForRequest(20);
	}
}

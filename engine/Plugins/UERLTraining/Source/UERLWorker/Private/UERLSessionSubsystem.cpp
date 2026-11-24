#include "UERLSessionSubsystem.h"

#include "UERLFrameGate.h"
#include "UERLStepPipeline.h"
#include "UERLWorkerLog.h"
#include "UERLWorkerRuntime.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

void UUERLSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	StartFromCommandLine();
}

EUERLCommandLineParseResult UUERLSessionSubsystem::ParseCommandLine(
	const TCHAR* CommandLine,
	FUERLWorkerLaunchConfig& OutConfig,
	FString& OutError)
{
	OutConfig = FUERLWorkerLaunchConfig();
	OutError.Reset();

	if (!FParse::Value(CommandLine, TEXT("uerlport="), OutConfig.Bridge.Port))
	{
		return EUERLCommandLineParseResult::Inactive;
	}

	int32 HandshakeTimeoutMs = static_cast<int32>(OutConfig.Bridge.HandshakeTimeoutMs);
	int32 RequestTimeoutMs = static_cast<int32>(OutConfig.Bridge.RequestTimeoutMs);
	FParse::Value(CommandLine, TEXT("uerlhandshaketimeoutms="), HandshakeTimeoutMs);
	FParse::Value(CommandLine, TEXT("uerlrequesttimeoutms="), RequestTimeoutMs);
	FParse::Value(CommandLine, TEXT("uerlperformancepath="), OutConfig.Bridge.PerformancePath);
	OutConfig.Bridge.HandshakeTimeoutMs = HandshakeTimeoutMs > 0 ? static_cast<uint32>(HandshakeTimeoutMs) : 0;
	OutConfig.Bridge.RequestTimeoutMs = RequestTimeoutMs > 0 ? static_cast<uint32>(RequestTimeoutMs) : 0;
	OutConfig.Ownership = FParse::Param(CommandLine, TEXT("uerlattach"))
		? EUERLSessionOwnership::Attached
		: EUERLSessionOwnership::Process;

	FString Presentation;
	if (!FParse::Value(CommandLine, TEXT("uerlpresentation="), Presentation))
	{
		OutError = TEXT("-uerlpresentation=none|viewport|gameplay is required when -uerlport is present");
		return EUERLCommandLineParseResult::Invalid;
	}
	Presentation.ToLowerInline();
	if (Presentation == TEXT("none"))
	{
		OutConfig.PresentationMode = EUERLPresentationMode::None;
		if (!FParse::Param(CommandLine, TEXT("nullrhi"))
			|| !FParse::Param(CommandLine, TEXT("unattended"))
			|| !FParse::Param(CommandLine, TEXT("nosound")))
		{
			OutError = TEXT("PresentationMode.NONE requires -nullrhi -unattended -nosound");
			return EUERLCommandLineParseResult::Invalid;
		}
	}
	else if (Presentation == TEXT("viewport"))
	{
		OutConfig.PresentationMode = EUERLPresentationMode::Viewport;
		if (FParse::Param(CommandLine, TEXT("nullrhi")))
		{
			OutError = TEXT("PresentationMode.VIEWPORT cannot be combined with -nullrhi");
			return EUERLCommandLineParseResult::Invalid;
		}
	}
	else if (Presentation == TEXT("gameplay"))
	{
		OutConfig.PresentationMode = EUERLPresentationMode::Gameplay;
		if (FParse::Param(CommandLine, TEXT("nullrhi")))
		{
			OutError = TEXT("PresentationMode.GAMEPLAY cannot be combined with -nullrhi");
			return EUERLCommandLineParseResult::Invalid;
		}
	}
	else
	{
		OutError = FString::Printf(TEXT("unknown -uerlpresentation value '%s'"), *Presentation);
		return EUERLCommandLineParseResult::Invalid;
	}

	if (!OutConfig.IsValid())
	{
		OutError = TEXT("Worker port and timeout values must be positive and in range");
		return EUERLCommandLineParseResult::Invalid;
	}
	return EUERLCommandLineParseResult::Valid;
}

bool UUERLSessionSubsystem::StartFromCommandLine()
{
	FUERLWorkerLaunchConfig Config;
	FString Error;
	const EUERLCommandLineParseResult Result = ParseCommandLine(FCommandLine::Get(), Config, Error);
	if (Result == EUERLCommandLineParseResult::Inactive)
	{
		UE_LOG(LogUERLWorker, Log, TEXT("[FLOW] no -uerlport; Worker session inactive"));
		return false;
	}
	if (Result == EUERLCommandLineParseResult::Invalid)
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] rejected Worker command line: %s"), *Error);
		if (Config.Ownership == EUERLSessionOwnership::Process)
		{
			FPlatformMisc::RequestExitWithStatus(
				true, UERLProcessExitCode::StartupFailure, TEXT("UUERLSessionSubsystem::StartFromCommandLine"));
		}
		return false;
	}
	const bool bStarted = Config.Ownership == EUERLSessionOwnership::Attached
		? StartAttached(Config, Error)
		: StartSession(Config, Error);
	if (!bStarted)
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] Worker session startup failed: %s"), *Error);
		if (Config.Ownership == EUERLSessionOwnership::Process)
		{
			FPlatformMisc::RequestExitWithStatus(
				true, UERLProcessExitCode::StartupFailure, TEXT("UUERLSessionSubsystem::StartFromCommandLine"));
		}
		return false;
	}
	return true;
}

bool UUERLSessionSubsystem::StartAttached(const FUERLWorkerLaunchConfig& Config, FString& OutError)
{
	FUERLWorkerLaunchConfig AttachedConfig = Config;
	AttachedConfig.Ownership = EUERLSessionOwnership::Attached;
	UE_LOG(LogUERLWorker, Display, TEXT("[FLOW] starting explicit attached Worker session"));
	return StartSession(AttachedConfig, OutError);
}

bool UUERLSessionSubsystem::StartSession(const FUERLWorkerLaunchConfig& Config, FString& OutError)
{
	if (bSessionActivated || FUERLWorkerRuntime::Get().IsActive())
	{
		OutError = TEXT("a Worker session is already active");
		return false;
	}
	if (!Config.IsValid())
	{
		OutError = TEXT("invalid Worker launch configuration");
		return false;
	}
	if (GEngine && GEngine->GetCustomTimeStep())
	{
		OutError = TEXT("the host already owns an Engine CustomTimeStep");
		return false;
	}

	TWeakObjectPtr<UUERLSessionSubsystem> WeakThis(this);
	if (!FUERLWorkerRuntime::Get().Activate(Config, [WeakThis](EUERLSessionEndReason Reason)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Reason]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->HandleSessionEnded(Reason);
			}
		});
	}))
	{
		OutError = TEXT("Worker Runtime activation failed; see UE log");
		return false;
	}

	FrameGate = NewObject<UUERLFrameGate>(this, TEXT("UERLFrameGate"));
	if (!GEngine || !FrameGate || !FUERLWorkerRuntime::Get().BindFrameGate(FrameGate))
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[VERIFY] cannot install Frame Gate"));
		FUERLWorkerRuntime::Get().Deactivate();
		FrameGate = nullptr;
		OutError = TEXT("cannot install request-driven Frame Gate");
		return false;
	}
	GEngine->SetCustomTimeStep(FrameGate);
	if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
		World && World->HasBegunPlay())
	{
		UUERLStepPipeline* Pipeline = World->GetSubsystem<UUERLStepPipeline>();
		if (!Pipeline || !Pipeline->ActivateForWorker())
		{
			GEngine->SetCustomTimeStep(nullptr);
			FUERLWorkerRuntime::Get().UnbindFrameGate(FrameGate);
			FrameGate = nullptr;
			FUERLWorkerRuntime::Get().Deactivate();
			OutError = TEXT("running Game/PIE World cannot activate its Worker Step Pipeline");
			return false;
		}
	}
	ActiveConfig = Config;
	bSessionActivated = true;
	UE_LOG(LogUERLWorker, Display,
		TEXT("[FLOW] Worker session started presentation=%s ownership=%s port=%d"),
		Config.PresentationMode == EUERLPresentationMode::None ? TEXT("NONE")
			: Config.PresentationMode == EUERLPresentationMode::Viewport ? TEXT("VIEWPORT") : TEXT("GAMEPLAY"),
		Config.Ownership == EUERLSessionOwnership::Process ? TEXT("PROCESS") : TEXT("ATTACHED"),
		Config.Bridge.Port);
	return true;
}

void UUERLSessionSubsystem::HandleSessionEnded(EUERLSessionEndReason Reason)
{
	if (!bSessionActivated)
	{
		return;
	}
	const EUERLSessionOwnership Ownership = ActiveConfig.Ownership;
	const bool bGraceful = Reason == EUERLSessionEndReason::GracefulShutdown;
	ShutdownSession();
	if (Ownership == EUERLSessionOwnership::Process)
	{
		const uint8 ExitCode = bGraceful
			? UERLProcessExitCode::Success
			: UERLProcessExitCode::RuntimeFailure;
		UE_LOG(LogUERLWorker, Display, TEXT("[VERIFY] Worker process exit requested code=%u reason=%d"),
			ExitCode, static_cast<int32>(Reason));
		// A process-owned Worker is terminal once its explicit Session resources are gone.
		// Exit immediately so unrelated Editor async work cannot delay the ownership boundary.
		FPlatformMisc::RequestExitWithStatus(
			true, ExitCode, TEXT("UUERLSessionSubsystem::HandleSessionEnded"));
	}
	else
	{
		UE_LOG(LogUERLWorker, Display,
			TEXT("[VERIFY] attached Worker session closed without terminating the host reason=%d"),
			static_cast<int32>(Reason));
	}
}

void UUERLSessionSubsystem::Shutdown()
{
	ShutdownSession();
}

void UUERLSessionSubsystem::ShutdownSession()
{
	if (!bSessionActivated)
	{
		return;
	}
	UUERLStepPipeline* Pipeline = nullptr;
	if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
	{
		Pipeline = World->GetSubsystem<UUERLStepPipeline>();
	}
	FUERLWorkerRuntime::Get().Deactivate();
	if (Pipeline)
	{
		Pipeline->DeactivateForWorker();
	}
	if (GEngine && FrameGate && GEngine->GetCustomTimeStep() == FrameGate)
	{
		GEngine->SetCustomTimeStep(nullptr);
	}
	FUERLWorkerRuntime::Get().UnbindFrameGate(FrameGate);
	FrameGate = nullptr;
	bSessionActivated = false;
	ActiveConfig = FUERLWorkerLaunchConfig();
	UE_LOG(LogUERLWorker, Display, TEXT("[FLOW] Worker session deinitialized"));
}

void UUERLSessionSubsystem::Deinitialize()
{
	Shutdown();
	Super::Deinitialize();
}

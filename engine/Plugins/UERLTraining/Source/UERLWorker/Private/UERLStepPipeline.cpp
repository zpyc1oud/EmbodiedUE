#include "UERLStepPipeline.h"

#include "UERLSessionConfig.h"
#include "UERLPhysicsSnapshot.h"
#include "UERLWorkerLog.h"
#include "UERLWorkerRuntime.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/PhysicsSettings.h"

UUERLStepPipeline::UUERLStepPipeline() = default;
UUERLStepPipeline::~UUERLStepPipeline() = default;

bool UUERLStepPipeline::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UUERLStepPipeline::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer);
}

void UUERLStepPipeline::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (bReady)
	{
		return;
	}

	BoundWorld = &InWorld;
	if (FUERLWorkerRuntime::Get().IsActive())
	{
		ActivateForWorker();
	}
}

bool UUERLStepPipeline::ActivateForWorker()
{
	if (bReady)
	{
		return FUERLWorkerRuntime::Get().BindPipeline(this);
	}
	if (!BoundWorld || !FUERLWorkerRuntime::Get().IsActive())
	{
		return false;
	}

	RunStartupGateChecks();
	RegisterPhysicsTicks();
	bReady = bTicksRegistered;
	if (!FUERLWorkerRuntime::Get().BindPipeline(this))
	{
		bReady = false;
		UnregisterPhysicsTicks();
		return false;
	}

	UE_LOG(LogUERLWorker, Display, TEXT("[VERIFY] Worker World '%s' ready gate=%s"),
		*BoundWorld->GetName(), bGatePassed ? TEXT("PASS") : *GateFailure);

	if (FUERLWorkerRuntime::Get().GetConfig().PresentationMode == EUERLPresentationMode::Viewport && !ViewportObserver)
	{
		ViewportObserver = MakeUnique<FUERLViewportObserver>();
		ViewportObserver->Install(*BoundWorld);
	}
	return bReady;
}

void UUERLStepPipeline::DeactivateForWorker()
{
	if (ViewportObserver)
	{
		ViewportObserver->Uninstall();
		ViewportObserver.Reset();
	}
	UnregisterPhysicsTicks();
	FUERLWorkerRuntime::Get().UnbindPipeline(this);
	bReady = false;
	bGatePassed = false;
	GateFailure.Reset();
}

void UUERLStepPipeline::Deinitialize()
{
	DeactivateForWorker();
	BoundWorld = nullptr;
	Super::Deinitialize();
}

void UUERLStepPipeline::RunStartupGateChecks()
{
	bGatePassed = true;
	GateFailure.Reset();
	auto Fail = [this](const FString& Reason)
	{
		bGatePassed = false;
		if (!GateFailure.IsEmpty()) { GateFailure += TEXT(";"); }
		GateFailure += Reason;
	};

	int32 ForceBadGate = 0;
	FParse::Value(FCommandLine::Get(), TEXT("uerlforcebadgate="), ForceBadGate);
	if (ForceBadGate != 0)
	{
		Fail(TEXT("forced gate failure via -uerlforcebadgate"));
	}

	const double PhysicsDt = FUERLWorkerRuntime::Get().GetPhysicsDt();
	const UPhysicsSettings* Settings = UPhysicsSettings::Get();
	if (!Settings)
	{
		Fail(TEXT("UPhysicsSettings unavailable"));
	}
	else
	{
		// The Worker locks the Chaos scene to one integration step per engine
		// frame through its launch arguments (WORKER_LOCKSTEP_PHYSICS_ARGS);
		// this gate only reads the effective settings and never rewrites them.
		if (Settings->bTickPhysicsAsync)
		{
			Fail(TEXT("bTickPhysicsAsync must be false; Workers lock physics via launch arguments"));
		}
		if (Settings->bSubstepping)
		{
			Fail(TEXT("bSubstepping must be false; Workers lock physics via launch arguments"));
		}
		if (Settings->bSubsteppingAsync)
		{
			Fail(TEXT("bSubsteppingAsync must be false; Workers lock physics via launch arguments"));
		}
		if (Settings->MaxPhysicsDeltaTime > 0.0f && Settings->MaxPhysicsDeltaTime < PhysicsDt)
		{
			Fail(FString::Printf(TEXT("MaxPhysicsDeltaTime %.6f would clamp physics_dt %.6f"),
				Settings->MaxPhysicsDeltaTime, PhysicsDt));
		}
	}

	if (BoundWorld && BoundWorld->GetWorldSettings())
	{
		const float Dilation = BoundWorld->GetWorldSettings()->GetEffectiveTimeDilation();
		if (!FMath::IsNearlyEqual(Dilation, 1.0f, 1e-4f))
		{
			Fail(FString::Printf(TEXT("time dilation %.6f must be 1.0"), Dilation));
		}
	}
}

bool UUERLStepPipeline::ValidateRequestedPhysics(double PhysicsDt, FString& OutError) const
{
	if (!bGatePassed)
	{
		OutError = GateFailure;
		return false;
	}
	if (!FMath::IsFinite(PhysicsDt) || PhysicsDt <= 0.0)
	{
		OutError = TEXT("physics_dt must be finite and positive");
		return false;
	}
	const UPhysicsSettings* Settings = UPhysicsSettings::Get();
	if (!Settings)
	{
		OutError = TEXT("UPhysicsSettings unavailable");
		return false;
	}
	if (Settings->MaxPhysicsDeltaTime > 0.0f && Settings->MaxPhysicsDeltaTime < PhysicsDt)
	{
		OutError = FString::Printf(TEXT("MaxPhysicsDeltaTime %.6f would clamp requested physics_dt %.6f"),
			Settings->MaxPhysicsDeltaTime, PhysicsDt);
		return false;
	}
	return true;
}

bool UUERLStepPipeline::ReadSolverFrameTime(int32& OutFrame, double& OutTime) const
{
	if (!BoundWorld)
	{
		return false;
	}
	FUERLSolverClockSnapshot Clock;
	FString Error;
	if (!ReadUERLSolverClock(*BoundWorld, Clock, Error))
	{
		return false;
	}
	OutFrame = Clock.Frame;
	OutTime = Clock.SolverTime;
	return true;
}

bool UUERLStepPipeline::CaptureSolverBaseline(FString& OutError)
{
	int32 Frame = 0;
	double Time = 0.0;
	if (!ReadSolverFrameTime(Frame, Time))
	{
		OutError = TEXT("Chaos solver is unavailable while capturing the Initialize baseline");
		return false;
	}
	LastAckSolverFrame = Frame;
	LastAckSolverTime = Time;
	UE_LOG(LogUERLWorker, Display, TEXT("[VERIFY] solver baseline frame=%d time=%.6f"), Frame, Time);
	return true;
}

bool UUERLStepPipeline::BeginStepMeasurement(FString& OutError)
{
	int32 Frame = 0;
	double Time = 0.0;
	if (!ReadSolverFrameTime(Frame, Time))
	{
		OutError = TEXT("Chaos solver is unavailable before a requested Step");
		return false;
	}
	const int32 IdleFrames = Frame - LastAckSolverFrame;
	const double IdleTime = Time - LastAckSolverTime;
	if (CompletedSteps < 3 || IdleFrames != 0)
	{
		UE_LOG(LogUERLWorker, Display, TEXT("[VERIFY] idle delta solverFrames=%d solverTime=%.6f"), IdleFrames, IdleTime);
	}
	if (IdleFrames != 0 || !FMath::IsNearlyZero(IdleTime, 1e-4))
	{
		OutError = FString::Printf(TEXT("training physics advanced while idle (frames=%d time=%.6f)"),
			IdleFrames, IdleTime);
		return false;
	}
	StepStartSolverFrame = Frame;
	StepStartSolverTime = Time;
	StepStartPhysicsFrame = PhysicsFrameCount;
	return true;
}

bool UUERLStepPipeline::EndStepMeasurement(FString& OutError)
{
	int32 Frame = 0;
	double Time = 0.0;
	if (!ReadSolverFrameTime(Frame, Time))
	{
		OutError = TEXT("Chaos solver is unavailable after a requested Step");
		return false;
	}
	const int64 PhysicsFrames = PhysicsFrameCount - StepStartPhysicsFrame;
	const int32 SolverFrames = Frame - StepStartSolverFrame;
	const double SolverTime = Time - StepStartSolverTime;
	const int32 ExpectedFrames = FUERLWorkerRuntime::Get().GetActiveStepDecimation();
	const double ExpectedTime = ExpectedFrames * FUERLWorkerRuntime::Get().GetPhysicsDt();
	const bool bMatches = PhysicsFrames == ExpectedFrames && SolverFrames == ExpectedFrames
		&& FMath::IsNearlyEqual(SolverTime, ExpectedTime, 1e-4);
	if (CompletedSteps < 3 || !bMatches)
	{
		UE_LOG(LogUERLWorker, Display,
			TEXT("[VERIFY] fixed step=%lld physicsFrames=%lld solverFrames=%d solverTime=%.6f expected=%d/%.6f match=%d"),
			CompletedSteps, PhysicsFrames, SolverFrames, SolverTime, ExpectedFrames, ExpectedTime, bMatches);
	}
	if (!bMatches)
	{
		OutError = FString::Printf(
			TEXT("fixed-step invariant failed (physics=%lld solver=%d time=%.6f expected=%d/%.6f)"),
			PhysicsFrames, SolverFrames, SolverTime, ExpectedFrames, ExpectedTime);
		return false;
	}
	LastAckSolverFrame = Frame;
	LastAckSolverTime = Time;
	++CompletedSteps;
	return true;
}

void UUERLStepPipeline::OnPrePhysics()
{
	FUERLWorkerRuntime& Runtime = FUERLWorkerRuntime::Get();
	if (Runtime.GetPhase() != EUERLWorkerPhase::ActiveStep)
	{
		return;
	}
	const bool bFirstPhysicsFrame = Runtime.GetRemainingFrames() == Runtime.GetActiveStepDecimation();
	if (bFirstPhysicsFrame)
	{
		FString Error;
		if (!BeginStepMeasurement(Error))
		{
			Runtime.FailActiveStep(Error);
			return;
		}
	}
	if (Runtime.ShouldApplyActiveCommands() && !Runtime.ApplyActiveCommands())
	{
		return;
	}
	Runtime.BeginPhysicsFrameTiming();
}

void UUERLStepPipeline::OnPostPhysics()
{
	FUERLWorkerRuntime& Runtime = FUERLWorkerRuntime::Get();
	if (ViewportObserver)
	{
		ViewportObserver->UpdateCamera(
			Runtime.GetPhase() == EUERLWorkerPhase::ActiveStep,
			Runtime.GetPhysicsDt());
	}
	if (Runtime.GetPhase() == EUERLWorkerPhase::StabilizingInitialize)
	{
		Runtime.FinishInitializeStabilization();
		return;
	}
	if (Runtime.GetPhase() != EUERLWorkerPhase::ActiveStep)
	{
		return;
	}
	Runtime.EndPhysicsFrameTiming();
	++PhysicsFrameCount;
	if (Runtime.CompletePhysicsFrame() == 0)
	{
		FString Error;
		if (!EndStepMeasurement(Error))
		{
			Runtime.FailActiveStep(Error);
			return;
		}
		Runtime.FinishActiveStep();
	}
}

void UUERLStepPipeline::RegisterPhysicsTicks()
{
	if (bTicksRegistered || !BoundWorld || !BoundWorld->PersistentLevel)
	{
		return;
	}
	PrePhysicsTick.Owner = this;
	PrePhysicsTick.bCanEverTick = true;
	PrePhysicsTick.bStartWithTickEnabled = true;
	PrePhysicsTick.TickGroup = TG_PrePhysics;
	PrePhysicsTick.RegisterTickFunction(BoundWorld->PersistentLevel);

	PostPhysicsTick.Owner = this;
	PostPhysicsTick.bCanEverTick = true;
	PostPhysicsTick.bStartWithTickEnabled = true;
	PostPhysicsTick.TickGroup = TG_PostPhysics;
	PostPhysicsTick.RegisterTickFunction(BoundWorld->PersistentLevel);
	bTicksRegistered = true;
}

void UUERLStepPipeline::UnregisterPhysicsTicks()
{
	if (!bTicksRegistered) { return; }
	if (PrePhysicsTick.IsTickFunctionRegistered()) { PrePhysicsTick.UnRegisterTickFunction(); }
	if (PostPhysicsTick.IsTickFunctionRegistered()) { PostPhysicsTick.UnRegisterTickFunction(); }
	PrePhysicsTick.Owner = nullptr;
	PostPhysicsTick.Owner = nullptr;
	bTicksRegistered = false;
}

void FUERLPrePhysicsTickFunction::ExecuteTick(float DeltaTime, ELevelTick TickType,
	ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	if (Owner) { Owner->OnPrePhysics(); }
}

FString FUERLPrePhysicsTickFunction::DiagnosticMessage()
{
	return TEXT("UERLPrePhysicsTickFunction");
}

void FUERLPostPhysicsTickFunction::ExecuteTick(float DeltaTime, ELevelTick TickType,
	ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	if (Owner) { Owner->OnPostPhysics(); }
}

FString FUERLPostPhysicsTickFunction::DiagnosticMessage()
{
	return TEXT("UERLPostPhysicsTickFunction");
}

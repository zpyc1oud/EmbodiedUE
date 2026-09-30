#include "UERLViewportRecorder.h"

#include "Async/Async.h"
#include "UERLWorkerLog.h"

#include "Engine/GameEngine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "RHI.h"
#include "FrameGrabber.h"
#include "RenderingThread.h"
#include "Serialization/Archive.h"
#include "Slate/SceneViewport.h"
#include "Slate/SlateViewportProvider.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

FUERLViewportRecorder::FUERLViewportRecorder() = default;
FUERLViewportRecorder::~FUERLViewportRecorder() = default;

bool FUERLViewportRecorder::Start(
	UWorld& World,
	const FString& InOutputDirectory,
	int32 InFramesPerSecond,
	int32 Width,
	int32 Height,
	int32 InMaxFrames)
{
	if (SceneViewport.IsValid())
	{
		return false;
	}
	if (InOutputDirectory.IsEmpty() || InFramesPerSecond < 1 || Width < 1 || Height < 1 || InMaxFrames < 1)
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[RECORD] invalid internal recorder settings"));
		return false;
	}

	UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
	if (!GameEngine || !GameEngine->SceneViewport.IsValid())
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[RECORD] UE game scene viewport is unavailable"));
		return false;
	}
	FSceneViewport* GameSceneViewport = GameEngine->SceneViewport.Get();
	if (!GameSceneViewport || World.GetGameViewport() == nullptr)
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[RECORD] game viewport is unavailable for the Worker world"));
		return false;
	}
	if (!FSlateApplication::IsInitialized() || FSlateApplication::Get().GetRenderer() == nullptr)
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[RECORD] UE Slate renderer is unavailable"));
		return false;
	}

	OutputDirectory = FPaths::ConvertRelativePathToFull(InOutputDirectory);
	if (!IFileManager::Get().MakeDirectory(*OutputDirectory, true))
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[RECORD] could not create output directory '%s'"), *OutputDirectory);
		return false;
	}

	FramesPerSecond = InFramesPerSecond;
	MaxFrames = InMaxFrames;
	CaptureSize = FIntPoint(Width, Height);
	RequestedFrames = 0;
	PendingCaptureRequests.Reset();
	CaptureAccumulator = 0.0;
	CaptureInterval = 1.0 / static_cast<double>(FramesPerSecond);
	LastBackBuffer = nullptr;
	CapturedFrames.Reset();
	WrittenFrames.Reset();
	bCapturing = false;
	bWriteFailed = false;
	WriteError.Reset();
	SurfaceReaders.Reset();
	CurrentSurfaceIndex = 0;
	bHasPreviousSurface = false;
	RawWriteTask.Reset();
	bRawWriterStopping = false;
	RawWriterEvent = FPlatformProcess::GetSynchEventFromPool(false);
	{
		FScopeLock Lock(&CapturedFramesMutex);
		PendingFrames.Reset();
	}

	SceneViewport = GameEngine->SceneViewport;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		TUniquePtr<FViewportSurfaceReader> Reader = MakeUnique<FViewportSurfaceReader>(PF_B8G8R8A8, CaptureSize);
		Reader->SetCaptureRect(FIntRect(0, 0, CaptureSize.X, CaptureSize.Y));
		Reader->SetWindowSize(CaptureSize);
		SurfaceReaders.Add(MoveTemp(Reader));
	}
	FlushRenderingCommands();
	TargetWindowPtr = nullptr;
	if (TSharedPtr<SViewport> ViewportWidget = GameSceneViewport->GetViewportWidget().Pin())
	{
		if (TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef()))
		{
			TargetWindowPtr = Window.Get();
		}
	}

	bCapturing = true;
	BackBufferReadyHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(
		this, &FUERLViewportRecorder::OnBackBufferReadyToPresent);
	RawWriteTask = Async(
		EAsyncExecution::Thread,
		[this]()
		{
			return WriteRawFrames();
		});

	UE_LOG(LogUERLWorker, Display,
		TEXT("[RECORD] internal UE capture started: directory='%s' size=%dx%d fps=%d max_frames=%d"),
		*OutputDirectory, Width, Height, FramesPerSecond, MaxFrames);
	return true;
}

void FUERLViewportRecorder::Stop()
{
	if (!SceneViewport.IsValid())
	{
		return;
	}

	bCapturing = false;
	if (BackBufferReadyHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().Remove(BackBufferReadyHandle);
		BackBufferReadyHandle.Reset();
	}
	PendingCaptureRequests.Reset();

	FlushRenderingCommands();
	FinalizePendingReadback();
	if (!bWriteFailed && RequestedFrames < 1)
	{
		SetWriteError(TEXT("internal recorder stopped before capturing a simulation frame"));
	}
	if (!bWriteFailed && CapturedFrames.GetValue() != RequestedFrames)
	{
		SetWriteError(FString::Printf(
			TEXT("internal recorder captured %d of %d requested frames"),
			CapturedFrames.GetValue(),
			RequestedFrames));
	}
	for (TUniquePtr<FViewportSurfaceReader>& Reader : SurfaceReaders)
	{
		Reader->Reset();
	}

	bRawWriterStopping = true;
	if (RawWriterEvent)
	{
		RawWriterEvent->Trigger();
	}
	if (RawWriteTask.IsValid())
	{
		RawWriteTask.Get();
		RawWriteTask.Reset();
	}
	{
		FScopeLock Lock(&CapturedFramesMutex);
		PendingFrames.Reset();
	}
	if (RawWriterEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(RawWriterEvent);
		RawWriterEvent = nullptr;
	}

	if (bWriteFailed)
	{
		FString Error;
		{
			FScopeLock Lock(&WriteErrorMutex);
			Error = WriteError;
		}
		WriteStatusFile(TEXT("recording.error"), Error);
		UE_LOG(LogUERLWorker, Error, TEXT("[RECORD] internal UE capture failed: %s"), *Error);
	}
	else
	{
		WriteStatusFile(
			TEXT("recording.done"),
			FString::Printf(
				TEXT("frames=%d\nfps=%d\nwidth=%d\nheight=%d\nformat=bgra8\nfile=frames.bgra\n"),
				WrittenFrames.GetValue(),
				FramesPerSecond,
				CaptureSize.X,
				CaptureSize.Y));
		UE_LOG(LogUERLWorker, Display,
			TEXT("[RECORD] internal UE capture finished: frames=%d"), WrittenFrames.GetValue());
	}

	SurfaceReaders.Reset();
	SceneViewport.Reset();
	TargetWindowPtr = nullptr;
	OutputDirectory.Reset();
	FramesPerSecond = 0;
	MaxFrames = 0;
	CaptureSize = FIntPoint::ZeroValue;
	RequestedFrames = 0;
	PendingCaptureRequests.Reset();
	CaptureAccumulator = 0.0;
	CaptureInterval = 0.0;
	LastBackBuffer = nullptr;
	WrittenFrames.Reset();
	CapturedFrames.Reset();
	bCapturing = false;
}

void FUERLViewportRecorder::Tick(bool bAdvanceSimulationTime, double PhysicsDt)
{
	if (bAdvanceSimulationTime && bCapturing && PhysicsDt > 0.0)
	{
		CaptureAccumulator += PhysicsDt;
		while (CaptureAccumulator + UE_DOUBLE_SMALL_NUMBER >= CaptureInterval && RequestedFrames < MaxFrames)
		{
			CaptureAccumulator -= CaptureInterval;
			++RequestedFrames;
			PendingCaptureRequests.Increment();
		}
	}
}

void FUERLViewportRecorder::OnBackBufferReadyToPresent(
	SWindow& SlateWindow,
	ISlateViewportProvider& ViewportProvider)
{
	if (!bCapturing || PendingCaptureRequests.GetValue() <= 0)
	{
		return;
	}
	if (TargetWindowPtr != nullptr && TargetWindowPtr != &SlateWindow)
	{
		return;
	}

	FRHITexture* BackBuffer = ViewportProvider.GetBackBufferResource();
	if (!BackBuffer)
	{
		SetWriteError(TEXT("UE renderer presented without a back buffer"));
		bCapturing = false;
		return;
	}

	LastBackBuffer = BackBuffer;
	PendingCaptureRequests.Decrement();

	check(!SurfaceReaders.IsEmpty());
	FViewportSurfaceReader* CurrentReader = SurfaceReaders[CurrentSurfaceIndex].Get();
	CurrentReader->BlockUntilAvailable();
	CurrentReader->Initialize();

	FViewportSurfaceReader* PreviousReader = nullptr;
	if (bHasPreviousSurface)
	{
		const int32 PreviousIndex = (CurrentSurfaceIndex + SurfaceReaders.Num() - 1) % SurfaceReaders.Num();
		PreviousReader = SurfaceReaders[PreviousIndex].Get();
	}

	CurrentReader->ResolveRenderTarget(
		PreviousReader,
		BackBuffer,
		[this](FColor* ColorBuffer, int32 Width, int32 Height)
		{
			QueueCapturedFrame(ColorBuffer, Width, Height, Width);
		});
	bHasPreviousSurface = true;
	CurrentSurfaceIndex = (CurrentSurfaceIndex + 1) % SurfaceReaders.Num();
}

void FUERLViewportRecorder::FinalizePendingReadback()
{
	if (bWriteFailed)
	{
		return;
	}
	const int32 Requested = RequestedFrames;
	const int32 Captured = CapturedFrames.GetValue();
	if (Captured == Requested)
	{
		return;
	}
	if (Requested < Captured || Requested - Captured != 1 || !bHasPreviousSurface || !LastBackBuffer.IsValid())
	{
		SetWriteError(FString::Printf(
			TEXT("internal recorder could not finalize %d of %d requested frames"),
			Captured,
			Requested));
		return;
	}

	const int32 FinalReaderIndex = CurrentSurfaceIndex;
	const int32 PreviousReaderIndex = (CurrentSurfaceIndex + SurfaceReaders.Num() - 1) % SurfaceReaders.Num();
	const FTextureRHIRef FinalBackBuffer = LastBackBuffer;
	ENQUEUE_RENDER_COMMAND(UERLViewportRecorderFinalizeReadback)(
		[this, FinalReaderIndex, PreviousReaderIndex, FinalBackBuffer](FRHICommandListImmediate&)
		{
			check(SurfaceReaders.IsValidIndex(FinalReaderIndex));
			check(SurfaceReaders.IsValidIndex(PreviousReaderIndex));
			FViewportSurfaceReader* FinalReader = SurfaceReaders[FinalReaderIndex].Get();
			FViewportSurfaceReader* PreviousReader = SurfaceReaders[PreviousReaderIndex].Get();
			FinalReader->BlockUntilAvailable();
			FinalReader->Initialize();
			FinalReader->ResolveRenderTarget(
				PreviousReader,
				FinalBackBuffer,
				[this](FColor* ColorBuffer, int32 Width, int32 Height)
				{
					QueueCapturedFrame(ColorBuffer, Width, Height, Width);
				});
		});
	FlushRenderingCommands();
}

void FUERLViewportRecorder::QueueCapturedFrame(
	FColor* ColorBuffer,
	int32 Width,
	int32 Height,
	int32 RowPitchInPixels)
{
	if (bWriteFailed || bRawWriterStopping)
	{
		return;
	}
	if (!ColorBuffer || Width < 1 || Height < 1 || RowPitchInPixels < Width)
	{
		SetWriteError(TEXT("UE renderer returned an empty captured frame"));
		return;
	}

	FPendingFrame Frame;
	Frame.Size = FIntPoint(Width, Height);
	Frame.Colors.SetNumUninitialized(Width * Height);
	if (RowPitchInPixels == Width)
	{
		FMemory::Memcpy(
			Frame.Colors.GetData(),
			ColorBuffer,
			Width * Height * sizeof(FColor));
	}
	else
	{
		for (int32 Row = 0; Row < Height; ++Row)
		{
			FMemory::Memcpy(
				Frame.Colors.GetData() + Row * Width,
				ColorBuffer + Row * RowPitchInPixels,
				Width * sizeof(FColor));
		}
	}
	{
		FScopeLock Lock(&CapturedFramesMutex);
		if (bWriteFailed || bRawWriterStopping)
		{
			return;
		}
		PendingFrames.Add(MoveTemp(Frame));
	}
	CapturedFrames.Increment();
	if (RawWriterEvent)
	{
		RawWriterEvent->Trigger();
	}
}

void FUERLViewportRecorder::SetWriteError(const FString& Error)
{
	{
		FScopeLock Lock(&WriteErrorMutex);
		if (!bWriteFailed)
		{
			WriteError = Error;
		}
	}
	bWriteFailed = true;
	bCapturing = false;
	PendingCaptureRequests.Reset();
	bRawWriterStopping = true;
	if (RawWriterEvent)
	{
		RawWriterEvent->Trigger();
	}
}

bool FUERLViewportRecorder::WriteRawFrames()
{
	const FString RawPath = FPaths::Combine(OutputDirectory, TEXT("frames.bgra"));
	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*RawPath));
	if (!Writer)
	{
		SetWriteError(FString::Printf(TEXT("could not open raw captured frame stream '%s'"), *RawPath));
		return false;
	}

	while (true)
	{
		if (bWriteFailed)
		{
			FScopeLock Lock(&CapturedFramesMutex);
			PendingFrames.Reset();
			return false;
		}
		TArray<FPendingFrame> Frames;
		{
			FScopeLock Lock(&CapturedFramesMutex);
			Swap(Frames, PendingFrames);
		}

		for (const FPendingFrame& Frame : Frames)
		{
			if (Frame.Size.X < 1 || Frame.Size.Y < 1 || Frame.Colors.Num() != Frame.Size.X * Frame.Size.Y)
			{
				SetWriteError(TEXT("UE returned an empty captured frame"));
				return false;
			}
			Writer->Serialize(
				const_cast<FColor*>(Frame.Colors.GetData()),
				static_cast<int64>(Frame.Colors.Num()) * sizeof(FColor));
			if (Writer->IsError())
			{
				SetWriteError(FString::Printf(TEXT("could not write raw captured frame stream '%s'"), *RawPath));
				return false;
			}
			WrittenFrames.Increment();
		}

		bool bPendingFrames = false;
		{
			FScopeLock Lock(&CapturedFramesMutex);
			bPendingFrames = !PendingFrames.IsEmpty();
		}
		if (bRawWriterStopping && !bPendingFrames)
		{
			break;
		}
		if (Frames.IsEmpty() && RawWriterEvent)
		{
			RawWriterEvent->Wait(100);
		}
	}
	Writer->Flush();
	if (Writer->IsError())
	{
		SetWriteError(FString::Printf(TEXT("could not flush raw captured frame stream '%s'"), *RawPath));
		return false;
	}
	return true;
}

void FUERLViewportRecorder::WriteStatusFile(const TCHAR* Filename, const FString& Contents) const
{
	const FString Path = FPaths::Combine(OutputDirectory, Filename);
	if (!FFileHelper::SaveStringToFile(Contents, *Path))
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[RECORD] could not write status file '%s'"), *Path);
	}
}

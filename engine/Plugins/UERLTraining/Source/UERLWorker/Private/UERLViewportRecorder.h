#pragma once

#include "Async/Future.h"
#include "CoreMinimal.h"
#include "HAL/Event.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "RHIResources.h"

class FSceneViewport;
struct FViewportSurfaceReader;
class UWorld;

/**
 * Captures the UE game viewport from the renderer back buffer.
 *
 * This is deliberately independent of the desktop window: the frame source is
 * the same Slate/UE render target that the game presents, not a screen region.
 * The recorder owns the render-thread readback and the output directory for
 * one play invocation.
 */
class FUERLViewportRecorder
{
public:
	FUERLViewportRecorder();
	~FUERLViewportRecorder();

	/** Start internal back-buffer capture for the world's game viewport. */
	bool Start(
		UWorld& World,
		const FString& OutputDirectory,
		int32 FramesPerSecond,
		int32 Width,
		int32 Height,
		int32 MaxFrames);

	/** Stop capture, flush pending GPU readbacks, and write completion status. */
	void Stop();

	/** Advance the fixed-time capture schedule without blocking simulation. */
	void Tick(bool bAdvanceSimulationTime, double PhysicsDt);

	bool IsActive() const { return SceneViewport.IsValid(); }

private:
	struct FPendingFrame;
	void OnBackBufferReadyToPresent(class SWindow& SlateWindow, class ISlateViewportProvider& ViewportProvider);
	void FinalizePendingReadback();
	void QueueCapturedFrame(FColor* ColorBuffer, int32 Width, int32 Height, int32 RowPitchInPixels);
	void SetWriteError(const FString& Error);
	bool WriteRawFrames();
	void WriteStatusFile(const TCHAR* Filename, const FString& Contents) const;

	TSharedPtr<FSceneViewport> SceneViewport;
	FDelegateHandle BackBufferReadyHandle;
	void* TargetWindowPtr = nullptr;
	TArray<TUniquePtr<FViewportSurfaceReader>> SurfaceReaders;
	int32 CurrentSurfaceIndex = 0;
	bool bHasPreviousSurface = false;
	int32 RequestedFrames = 0;
	FThreadSafeCounter PendingCaptureRequests;
	double CaptureAccumulator = 0.0;
	double CaptureInterval = 0.0;
	FIntPoint CaptureSize = FIntPoint::ZeroValue;
	FTextureRHIRef LastBackBuffer;
	FCriticalSection CapturedFramesMutex;
	struct FPendingFrame
	{
		FIntPoint Size = FIntPoint::ZeroValue;
		TArray<FColor> Colors;
	};
	TArray<FPendingFrame> PendingFrames;
	TFuture<bool> RawWriteTask;
	FEvent* RawWriterEvent = nullptr;
	FThreadSafeBool bRawWriterStopping = false;
	FThreadSafeBool bCapturing = false;
	FThreadSafeCounter CapturedFrames;
	FString OutputDirectory;
	int32 FramesPerSecond = 0;
	int32 MaxFrames = 0;
	FThreadSafeCounter WrittenFrames;
	FThreadSafeBool bWriteFailed = false;
	FCriticalSection WriteErrorMutex;
	FString WriteError;
};

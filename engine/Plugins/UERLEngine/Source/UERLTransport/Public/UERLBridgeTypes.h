#pragma once

// Transport/Worker seam types. Staging PODs are owned by UERLInterface
// (UERLBatchStaging.h); this header must not redefine them.
#include "UERLBatchStaging.h"
#include "UERLInterfaceTypes.h"

/** Provide a mutable per-Slot fault-code view over one Worker staging batch. */
struct FUERLFaultBatchView
{
	/** Point at the first fault code without taking ownership. */
	uint8* Data = nullptr;
	/** Store the number of Slot rows in the view. */
	int32 NumRows = 0;

	/** Return whether the view points at a non-empty fault batch. */
	bool IsValid() const { return Data && NumRows > 0; }
	/** Return one writable per-Slot fault code without bounds checking. */
	uint8& At(int32 Row) const { return Data[Row]; }
};

/** Carry the validated Worker-domain configuration projected through the Bridge. */
struct FUERLWorkerProjection
{
	/** Store the number of stable Slot rows in the run. */
	int32 NumSlots = 0;
	/** Store the selected Action staging width. */
	int32 ActionWidth = 0;
	/** Store the selected State staging width. */
	int32 StateWidth = 0;
	/** Store the physical command width expected by the Worker. */
	int32 CommandWidth = 0;
	/** Store the run seed used by deterministic Worker sampling. */
	uint64 RunSeed = 0;
	/** Identify the UE World package loaded for this Run. */
	FString WorldMap;
	/** Store the requested fixed physics delta time. */
	double PhysicsDt = 1.0 / 60.0;
	/** Store the inclusive minimum physics-frame count per accepted Step. */
	int32 DecimationMin = 1;
	/** Store the inclusive maximum physics-frame count per accepted Step. */
	int32 DecimationMax = 1;
	/** Identify the registered Environment implementation. */
	FName EnvironmentId;
	/** Identify the registered Robot implementation. */
	FName RobotId;
	/** Store the validated Environment provider configuration. */
	FUERLProviderConfig EnvironmentConfig;
	/** Store the validated Robot provider configuration. */
	FUERLProviderConfig RobotConfig;
	/** Carry the optional pre-generated terrain grid; empty when absent. */
	FUERLTerrainConfig TerrainConfig;

	/** Validate the complete projection required by layout compilation. */
	bool IsValid() const
	{
		return IsWorkerConfigValid() && ActionWidth > 0 && StateWidth > 0 && CommandWidth >= 0;
	}

	/** Validate Worker timing, Slot count, provider identities, and optional terrain. */
	bool IsWorkerConfigValid() const
	{
		return NumSlots > 0 && FMath::IsFinite(PhysicsDt) && PhysicsDt > 0.0
			&& DecimationMin > 0 && DecimationMax >= DecimationMin
			&& FMath::IsFinite(PhysicsDt * static_cast<double>(DecimationMax))
			&& !WorldMap.IsEmpty()
			&& !EnvironmentId.IsNone() && !RobotId.IsNone()
			&& (TerrainConfig.NumLevels == 0 || TerrainConfig.IsValid());
	}
};

/** Identify the domain request submitted across the Transport/Worker seam. */
enum class EUERLBridgeRequestType : uint8
{
	PrepareInitialize,
	CommitInitialize,
	AbortInitialize,
	Ready,
	Step,
	Reset,
	Event,
	Shutdown,
};

/** Define stable Worker error categories returned through the Bridge seam. */
namespace UERLBridgeError
{
	constexpr int32 Ok = 0;
	constexpr int32 VersionMismatch = 1;
	constexpr int32 ConfigRejected = 2;
	constexpr int32 TransportFailure = 3;
	constexpr int32 ProtocolViolation = 4;
	constexpr int32 PhysicsInvalid = 5;
	constexpr int32 WorkerFatal = 6;
	constexpr int32 NotReady = 7;
	constexpr int32 AlreadyInflight = 8;
	constexpr int32 ShuttingDown = 9;
}

/** Carry exclusive Step timings across the Transport/Worker seam. */
struct FUERLStepTiming
{
	double SubmittedAtSeconds = 0.0;
	double ActionDecodeSeconds = 0.0;
	double QueueWaitSeconds = 0.0;
	double ActionApplySeconds = 0.0;
	int32 ActionApplyCount = 0;
	double PhysicsFrameSeconds = 0.0;
	double ContactSampleSeconds = 0.0;
	double StateCollectSeconds = 0.0;
};

/** Carry exclusive Reset timings across the Transport/Worker seam. */
struct FUERLResetTiming
{
	double SubmittedAtSeconds = 0.0;
	double RequestDecodeSeconds = 0.0;
	double QueueWaitSeconds = 0.0;
	double ResetSlotsSeconds = 0.0;
	double StateCollectSeconds = 0.0;
	double ValidateSlotsSeconds = 0.0;
	double SafetySeconds = 0.0;
	double EpisodeCopySeconds = 0.0;
};

/** Carry one fully typed request or response across the Transport/Worker seam. */
struct FUERLBridgeRequest
{
	/** Identify the operation the Worker must process. */
	EUERLBridgeRequestType Type = EUERLBridgeRequestType::PrepareInitialize;
	/** Correlate the request with the active Bridge transaction. */
	uint64 Sequence = 0;
	/** Carry the remaining transaction deadline in milliseconds. */
	uint32 TimeoutMs = 30000;
	/** Carry the validated Worker projection for initialization. */
	FUERLWorkerProjection Projection;
	/** Carry the selected schema used by staging and wire codecs. */
	FUERLBatchSchema SelectedSchema;
	/** View the decoded Action batch without taking ownership. */
	FUERLConstBatchView Actions;
	/** Carry the one int32 decimation value encoded in this Step frame. */
	int32 StepDecimation = 0;
	/** View the writable State staging batch without taking ownership. */
	FUERLMutableBatchView States;
	/** View the writable per-Slot fault batch without taking ownership. */
	FUERLFaultBatchView Faults;
	/** Identify Slots selected by a Reset request. */
	TArray<int32> ResetSlots;
	/** Carry the requested terrain level for every Slot during Reset. */
	TArray<uint16> TerrainLevels;
	/** Carry the fixed-layout Robot reset values in row-major Slot order. */
	TArray<float> ResetValues;
	/** Carry one selected-Slot scene/physics event batch. */
	FUERLEventBatch Event;
	/** Carry the current per-Slot episode indices. */
	TArray<uint64> EpisodeIndices;
	/** Carry the persisted Manifest hash for Ready acknowledgement. */
	FString ManifestHash;
	/** Carry process-local timings for a Step without changing the wire protocol. */
	FUERLStepTiming StepTiming;
	/** Carry process-local timings for a Reset without changing the wire protocol. */
	FUERLResetTiming ResetTiming;

	/** Store fields published by the Worker during PrepareInitialize. */
	TArray<FUERLFieldDescriptor> AvailableActionFields;
	TArray<FUERLFieldDescriptor> AvailableStateFields;
	/** Store the reflected robot topology reported during PrepareInitialize. */
	FUERLRobotTopology AvailableTopology;

	/** Report whether the Worker accepted the request. */
	bool bOk = false;
	/** Store the stable error category when bOk is false. */
	int32 ErrorCode = UERLBridgeError::WorkerFatal;
	/** Store a diagnostic that is safe to return through the Bridge. */
	FString ErrorMessage;

	/** Mark the request successful and clear any previous error. */
	void Succeed()
	{
		bOk = true;
		ErrorCode = UERLBridgeError::Ok;
		ErrorMessage.Reset();
	}

	/** Mark the request failed with a stable code and diagnostic. */
	void Fail(int32 Code, const FString& Message)
	{
		bOk = false;
		ErrorCode = Code;
		ErrorMessage = Message;
	}
};

/**
 * Define the Transport-to-Worker request seam.
 *
 * Implementations own request completion and must not expose Worker types to
 * the Transport module.
 */
class UERLTRANSPORT_API IBridgeRequestHandler
{
public:
	virtual ~IBridgeRequestHandler() = default;
	/** Submit one request and block the caller until the Worker completes it. */
	virtual void SubmitAndWait(FUERLBridgeRequest& InOutRequest) = 0;
	/** Notify the Worker that the Bridge connection ended unexpectedly. */
	virtual void NotifyBridgeDisconnected() = 0;
	/** Notify the Worker that a graceful Shutdown response was sent. */
	virtual void NotifyBridgeShutdownComplete() = 0;
};

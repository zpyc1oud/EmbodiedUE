#pragma once

#include "CoreMinimal.h"
#include "UERLBridgeServer.h"

/** UE presentation is a Session concern and never changes Worker task semantics. */
enum class EUERLPresentationMode : uint8
{
	None,
	Viewport,
	Gameplay,
};

/** Identifies whether the Worker may terminate the hosting UE process. */
enum class EUERLSessionOwnership : uint8
{
	Process,
	Attached,
};

/** Identify why a Worker session ended and whether it was expected. */
enum class EUERLSessionEndReason : uint8
{
	GracefulShutdown,
	BridgeDisconnected,
	WorkerFatal,
};

/** Identify the result of parsing the optional Worker command line. */
enum class EUERLCommandLineParseResult : uint8
{
	Inactive,
	Valid,
	Invalid,
};

namespace UERLProcessExitCode
{
	/** Return the process code used for a graceful Worker shutdown. */
	constexpr uint8 Success = 0;
	/** Return the process code used for a runtime Worker failure. */
	constexpr uint8 RuntimeFailure = 1;
	/** Return the process code used for a startup/configuration failure. */
	constexpr uint8 StartupFailure = 2;
}

/** Carry process/session configuration; wire Worker Config remains Transport-owned. */
struct FUERLWorkerLaunchConfig
{
	/** Configure the SocketBridge listener and request deadlines. */
	FUERLBridgeServerConfig Bridge;
	/** Select whether UE runs headless or with a viewport. */
	EUERLPresentationMode PresentationMode = EUERLPresentationMode::None;
	/** Select whether the Worker may terminate the hosting UE process. */
	EUERLSessionOwnership Ownership = EUERLSessionOwnership::Process;

	/** Validate the process/session configuration before activation. */
	bool IsValid() const { return Bridge.IsValid(); }
};

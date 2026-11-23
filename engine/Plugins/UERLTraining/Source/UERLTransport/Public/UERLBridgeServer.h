#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "UERLBridgeTypes.h"

/** Configure the transport-owned SocketBridge listener and request deadlines. */
struct FUERLBridgeServerConfig
{
	/** Store the loopback TCP port used by the listener. */
	int32 Port = 0;
	/** Bound the Hello transaction, including connection acceptance. */
	uint32 HandshakeTimeoutMs = 30000;
	/** Bound each post-Hello request transaction. */
	uint32 RequestTimeoutMs = 30000;
	/** Optionally persist one structured timing row per successful Step. */
	FString PerformancePath;

	/** Validate the listener port and positive transaction deadlines. */
	bool IsValid() const
	{
		return Port > 0 && Port <= 65535 && HandshakeTimeoutMs > 0 && RequestTimeoutMs > 0;
	}
};

/** Carry read-only Hello capabilities supplied by the Session owner. */
struct FUERLBridgeCapabilities
{
	/** Report whether the Worker can run without a viewport. */
	bool bHeadless = true;
	/** Report whether the Worker can expose a viewport. */
	bool bViewport = true;
	/** Report the active presentation mode without re-parsing Session config. */
	FString ActivePresentationMode = TEXT("none");
};

/**
 * Deep Transport module entry point. The SocketBridge owns framing, protocol
 * state, schema/layout compilation and batch serialization.
 */
class UERLTRANSPORT_API FUERLBridgeServer
{
public:
	FUERLBridgeServer();
	~FUERLBridgeServer();

	FUERLBridgeServer(const FUERLBridgeServer&) = delete;
	FUERLBridgeServer& operator=(const FUERLBridgeServer&) = delete;

	/**
	 * Start the single-client SocketBridge listener and its I/O thread.
	 *
	 * The server owns framing and protocol state while Handler owns Worker
	 * domain execution. A failed start leaves the server stopped.
	 *
	 * @param Config Supply the listener port and transaction deadlines.
	 * @param Capabilities Supply the read-only Hello capability projection.
	 * @param Handler Receive validated domain requests on the Worker seam.
	 * @param OutError Receive a diagnostic when startup fails.
	 * @return true when the listener and I/O thread are active.
	 */
	bool Start(
		const FUERLBridgeServerConfig& Config,
		const FUERLBridgeCapabilities& Capabilities,
		IBridgeRequestHandler& Handler,
		FString& OutError);
	/** Stop the listener, wait for the I/O thread, and release transport resources. */
	void Stop();
	/** Return whether the listener thread is currently active. */
	bool IsRunning() const;

private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};

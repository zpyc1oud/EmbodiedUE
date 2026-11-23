#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UERLSessionConfig.h"
#include "UERLSessionSubsystem.generated.h"

class UUERLFrameGate;

/** GameInstance entry point for launch/attach-independent Worker assembly. */
UCLASS()
class UERLWORKER_API UUERLSessionSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Initialize the subsystem and start a command-line Worker when requested. */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	/** Shut down the active Worker session before subsystem destruction. */
	virtual void Deinitialize() override;

	/**
	 * Parse the optional Worker command-line contract.
	 *
	 * @param CommandLine Supply the complete UE command line.
	 * @param OutConfig Receive the parsed process/session configuration.
	 * @param OutError Receive a diagnostic for an invalid active configuration.
	 * @return whether the Worker is inactive, valid, or invalid.
	 */
	static EUERLCommandLineParseResult ParseCommandLine(
		const TCHAR* CommandLine,
		FUERLWorkerLaunchConfig& OutConfig,
		FString& OutError);

	/** Parse and start the Worker session requested by the UE command line. */
	bool StartFromCommandLine();
	/** Start an attached Worker session without taking host process ownership. */
	bool StartAttached(const FUERLWorkerLaunchConfig& Config, FString& OutError);
	/** Shut down the active session and release its frame-control resources. */
	void Shutdown();
	/** Return whether this subsystem currently owns an active Worker session. */
	bool IsSessionActive() const { return bSessionActivated; }
	/** Return the configuration of the active session. */
	const FUERLWorkerLaunchConfig& GetActiveConfig() const { return ActiveConfig; }

private:
	bool StartSession(const FUERLWorkerLaunchConfig& Config, FString& OutError);
	void HandleSessionEnded(EUERLSessionEndReason Reason);
	void ShutdownSession();

	bool bSessionActivated = false;
	FUERLWorkerLaunchConfig ActiveConfig;

	UPROPERTY(Transient)
	TObjectPtr<UUERLFrameGate> FrameGate = nullptr;
};

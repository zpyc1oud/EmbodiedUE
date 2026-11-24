#include "Modules/ModuleManager.h"
#include "UERLAuthoredPursuitEnvironment.h"
#include "UERLIsolatedGridEnvironment.h"
#include "UERLRegistry.h"
#include "UERLSharedMapEnvironment.h"
#include "UERLWorkerLog.h"

DEFINE_LOG_CATEGORY(LogUERLWorker);

class FUERLWorkerModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		bAuthoredPursuitRegistered = FUERLEnvironmentRegistry::Get().RegisterFactory(
			UERLAuthoredPursuit::MakeFactory(), Error);
		if (!bAuthoredPursuitRegistered)
		{
			UE_LOG(LogUERLWorker, Error, TEXT("authored-pursuit Environment registration failed: %s"), *Error);
		}
		Error.Reset();
		bSharedMapRegistered = FUERLEnvironmentRegistry::Get().RegisterFactory(
			UERLSharedMap::MakeFactory(), Error);
		if (!bSharedMapRegistered)
		{
			UE_LOG(LogUERLWorker, Error, TEXT("shared-world Environment registration failed: %s"), *Error);
		}
		Error.Reset();
		bIsolatedGridRegistered = FUERLEnvironmentRegistry::Get().RegisterFactory(
			UERLIsolatedGrid::MakeFactory(), Error);
		if (!bIsolatedGridRegistered)
		{
			UE_LOG(LogUERLWorker, Error, TEXT("slot-isolated Environment registration failed: %s"), *Error);
		}
		UE_LOG(LogUERLWorker, Log, TEXT("[FLOW] UERLWorker module started"));
	}

	virtual void ShutdownModule() override
	{
		if (bIsolatedGridRegistered)
		{
			FUERLEnvironmentRegistry::Get().UnregisterFactory(UERLIsolatedGrid::EnvironmentId);
			bIsolatedGridRegistered = false;
		}
		if (bSharedMapRegistered)
		{
			FUERLEnvironmentRegistry::Get().UnregisterFactory(UERLSharedMap::EnvironmentId);
			bSharedMapRegistered = false;
		}
		if (bAuthoredPursuitRegistered)
		{
			FUERLEnvironmentRegistry::Get().UnregisterFactory(UERLAuthoredPursuit::EnvironmentId);
			bAuthoredPursuitRegistered = false;
		}
		UE_LOG(LogUERLWorker, Log, TEXT("[FLOW] UERLWorker module shutdown"));
	}

private:
	bool bAuthoredPursuitRegistered = false;
	bool bSharedMapRegistered = false;
	bool bIsolatedGridRegistered = false;
};

IMPLEMENT_MODULE(FUERLWorkerModule, UERLWorker);

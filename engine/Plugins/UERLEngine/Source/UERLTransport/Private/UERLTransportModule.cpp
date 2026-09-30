#include "Modules/ModuleManager.h"
#include "UERLTransportLog.h"

DEFINE_LOG_CATEGORY(LogUERLTransport);

class FUERLTransportModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogUERLTransport, Log, TEXT("[FLOW] UERLTransport module started"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogUERLTransport, Log, TEXT("[FLOW] UERLTransport module shutdown"));
	}
};

IMPLEMENT_MODULE(FUERLTransportModule, UERLTransport);

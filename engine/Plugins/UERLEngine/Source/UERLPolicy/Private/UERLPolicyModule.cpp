#include "Modules/ModuleManager.h"

#include "UERLPolicyLog.h"
#include "UERLPolicyOperators.h"

DEFINE_LOG_CATEGORY(LogUERLPolicy);

class FUERLPolicyModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterBuiltinPlanOperators();
	}
};

IMPLEMENT_MODULE(FUERLPolicyModule, UERLPolicy);

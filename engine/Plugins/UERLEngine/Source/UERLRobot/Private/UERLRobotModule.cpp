#include "Modules/ModuleManager.h"

#include "UERLGenericRobotProvider.h"
#include "UERLRobotLog.h"
#include "UERLRegistry.h"

DEFINE_LOG_CATEGORY(LogUERLRobot);

class FUERLRobotModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FUERLRobotRegistry::Get().RegisterFactory(UERLGenericRobot::MakeFactory(), Error))
		{
			UE_LOG(LogUERLRobot, Error, TEXT("[VERIFY] generic Robot registration failed: %s"), *Error);
			return;
		}
		bRegistered = true;
		UE_LOG(LogUERLRobot, Display, TEXT("[FLOW] registered %s"), *UERLGenericRobot::RobotId.ToString());
	}

	virtual void ShutdownModule() override
	{
		if (bRegistered)
		{
			FUERLRobotRegistry::Get().UnregisterFactory(UERLGenericRobot::RobotId);
			bRegistered = false;
		}
	}

private:
	bool bRegistered = false;
};

IMPLEMENT_MODULE(FUERLRobotModule, UERLRobot);

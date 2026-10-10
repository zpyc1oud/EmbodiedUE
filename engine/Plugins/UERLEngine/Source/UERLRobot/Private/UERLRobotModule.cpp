#include "Modules/ModuleManager.h"

#include "UERLGenericRobotProvider.h"
#include "UERLRobotLog.h"
#include "UERLRegistry.h"
#include "UERLRayGroundInput.h"

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
		bRayRegistered = FUERLInputRegistry::Get().RegisterFactory(MakeUERLRayGroundInputFactory(), Error);
		if (!bRayRegistered)
		{
			UE_LOG(LogUERLRobot, Error, TEXT("ground input registration failed: %s"), *Error);
		}
		UE_LOG(LogUERLRobot, Display, TEXT("[FLOW] registered %s"), *UERLGenericRobot::RobotId.ToString());
	}

	virtual void ShutdownModule() override
	{
		if (bRayRegistered)
		{
			FUERLInputRegistry::Get().UnregisterFactory(TEXT("uerl.ray_ground"), 1);
			bRayRegistered = false;
		}
		if (bRegistered)
		{
			FUERLRobotRegistry::Get().UnregisterFactory(UERLGenericRobot::RobotId);
			bRegistered = false;
		}
	}

private:
	bool bRegistered = false;
	bool bRayRegistered = false;
};

IMPLEMENT_MODULE(FUERLRobotModule, UERLRobot);

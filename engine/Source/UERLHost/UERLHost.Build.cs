// UERLHost 最小 Worker 宿主模块构建规则。
using UnrealBuildTool;

public class UERLHost : ModuleRules
{
	public UERLHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UERLPolicy",
			"UERLRobot",
		});
	}
}

using UnrealBuildTool;

public class UERLRobot : ModuleRules
{
	public UERLRobot(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"PhysicsCore",
			"UERLInterface",
			"UERLProvider",
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Chaos",
			"Json",
			"TraceLog",
		});
	}
}

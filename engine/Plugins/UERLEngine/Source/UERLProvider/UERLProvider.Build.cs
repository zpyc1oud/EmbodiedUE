using UnrealBuildTool;

public class UERLProvider : ModuleRules
{
	public UERLProvider(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UERLInterface",
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"PhysicsCore",
			"Json",
			"Chaos",
		});
	}
}
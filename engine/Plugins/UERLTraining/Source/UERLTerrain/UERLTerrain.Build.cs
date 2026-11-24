using UnrealBuildTool;

public class UERLTerrain : ModuleRules
{
	public UERLTerrain(ReadOnlyTargetRules Target) : base(Target)
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
			"Chaos",
			"Json",
			"ProceduralMeshComponent",
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}

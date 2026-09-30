using UnrealBuildTool;

public class UERLWorker : ModuleRules
{
	public UERLWorker(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UERLInterface",
			"UERLProvider",
			"UERLTerrain",
			"UERLTransport",
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"PhysicsCore",
			"Chaos",
			"UERLRobot",
			"Json",
			"MovieSceneCapture",
			"ProceduralMeshComponent",
			"RenderCore",
			"RHI",
			"Slate",
			"SlateCore",
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}

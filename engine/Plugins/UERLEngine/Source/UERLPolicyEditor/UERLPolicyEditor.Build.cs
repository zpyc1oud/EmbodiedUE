using UnrealBuildTool;

public class UERLPolicyEditor : ModuleRules
{
	public UERLPolicyEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UERLPolicy",
			"UERLRobot",
		});
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"AssetTools",
			"EditorFramework",
			"EditorSubsystem",
			"MessageLog",
			"Projects",
			"Slate",
			"SlateCore",
			"UnrealEd",
		});
	}
}

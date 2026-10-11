using UnrealBuildTool;

public class ExampleInput : ModuleRules
{
    public ExampleInput(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "UERLProvider", "Json" });
    }
}

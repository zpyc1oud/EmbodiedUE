using UnrealBuildTool;

public class UERLInterface : ModuleRules
{
	public UERLInterface(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.Add("Core");

		// Json is required for FUERLObservationPlan::ParseJson / FUERLActionPlan::ParseJson.
		// Move these types to UERLPolicy (ticket 05) and drop this dependency if Interface
		// no longer parses plan JSON.
		PrivateDependencyModuleNames.Add("Json");
	}
}

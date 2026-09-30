using UnrealBuildTool;

public class UERLPolicy : ModuleRules
{
	public UERLPolicy(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Plan types and FUERLFieldDescriptor live in UERLInterface (landed tickets
		// 02 / 13). Do not depend on UERLWorker or UERLTransport.
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UERLInterface",
			"UERLRobot",
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Json",
			// NNE interface + ORT CPU runtime (ticket 14). Runtime is also
			// declared in UERLEngine.uplugin so GetRuntime("NNERuntimeORTCpu") works.
			"NNE",
			"NNERuntimeORT",
		});

		// Test TUs share anonymous-namespace helper names (LoadJsonObject, etc.);
		// unity blending them into one Module.UERLPolicy.cpp hits C2084.
		bUseUnity = false;
	}
}

using UnrealBuildTool;

public class UERLTransport : ModuleRules
{
	public UERLTransport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"UERLInterface",
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"CoreUObject",
			"Sockets",
			"Networking",
			"Json",
			"Projects",
		});

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}

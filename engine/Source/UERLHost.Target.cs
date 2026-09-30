// UERLHost 最小 Worker 宿主游戏 Target。
using UnrealBuildTool;
using System.Collections.Generic;

public class UERLHostTarget : TargetRules
{
	public UERLHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UERLHost");
	}
}

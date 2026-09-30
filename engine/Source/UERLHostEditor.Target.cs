// UERLHost 编辑器与命令行测试 Target。
using UnrealBuildTool;
using System.Collections.Generic;

public class UERLHostEditorTarget : TargetRules
{
	public UERLHostEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("UERLHost");
	}
}

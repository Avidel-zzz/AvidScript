using UnrealBuildTool;

public class AvidScriptEditor : ModuleRules
{
	public AvidScriptEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"AvidScriptRuntime"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AvidScriptCore",
				"AvidScriptBindings",
				"Projects",
				"Json",
				"UnrealEd",
				"ToolMenus",
				"Slate",
				"SlateCore",
				"LevelEditor"
			}
		);
	}
}
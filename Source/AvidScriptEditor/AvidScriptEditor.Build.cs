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
				"AvidScriptCore",
				"AvidScriptRuntime"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AvidScriptBindings",
				"AvidScriptVM",
				"DirectoryWatcher",
				"Projects",
				"Json",
				"Kismet",
				"UnrealEd",
				"ToolMenus",
				"Slate",
				"SlateCore",
				"LevelEditor"
			}
		);
	}
}

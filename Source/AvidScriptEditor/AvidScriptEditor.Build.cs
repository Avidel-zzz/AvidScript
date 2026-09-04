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
				"AssetRegistry",
				"AvidScriptBindings",
				"AvidScriptVM",
				"BlueprintGraph",
				"DirectoryWatcher",
				"InputCore",
				"Projects",
				"Json",
				"Kismet",
				"UnrealEd",
				"UMG",
				"UMGEditor",
				"ToolMenus",
				"TraceLog",
				"Slate",
				"SlateCore",
				"LevelEditor",
				"MessageLog"
			}
		);

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}

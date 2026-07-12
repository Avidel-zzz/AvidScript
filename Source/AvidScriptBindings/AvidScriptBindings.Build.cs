using UnrealBuildTool;

public class AvidScriptBindings : ModuleRules
{
	public AvidScriptBindings(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"AvidScriptCore",
				"Core",
				"CoreUObject",
				"Engine"
			}
		);
	}
}

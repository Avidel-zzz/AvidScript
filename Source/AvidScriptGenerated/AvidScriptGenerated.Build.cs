using UnrealBuildTool;

public class AvidScriptGenerated : ModuleRules
{
	public AvidScriptGenerated(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AvidScriptRuntime",
				"Projects"
			}
		);
	}
}

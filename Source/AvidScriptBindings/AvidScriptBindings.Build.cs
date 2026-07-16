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
				"AvidScriptVM",
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.Add("Json");
	}
}

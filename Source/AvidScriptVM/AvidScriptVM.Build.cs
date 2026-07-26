using UnrealBuildTool;

public class AvidScriptVM : ModuleRules
{
	public AvidScriptVM(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"AvidScriptCore",
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"Projects",
				"WAMR",
				"Wasmtime"
			}
		);
	}
}

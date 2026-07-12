using UnrealBuildTool;

public class AvidScriptCore : ModuleRules
{
    public AvidScriptCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core"
            }
        );
    }
}

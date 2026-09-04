using UnrealBuildTool;

public class AvidScriptValidation : ModuleRules
{
    public AvidScriptValidation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "Json", "UMG",
            "AvidScriptCore", "AvidScriptRuntime", "AvidScriptVM"
        });
    }
}

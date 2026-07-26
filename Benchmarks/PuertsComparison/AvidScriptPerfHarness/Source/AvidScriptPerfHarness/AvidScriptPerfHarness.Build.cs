using UnrealBuildTool;

public class AvidScriptPerfHarness : ModuleRules
{
    public AvidScriptPerfHarness(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "JsEnv"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AvidScriptBindings",
            "AvidScriptEditor",
            "AvidScriptRuntime",
            "Json",
            "JsonUtilities",
            "Projects"
        });
    }
}

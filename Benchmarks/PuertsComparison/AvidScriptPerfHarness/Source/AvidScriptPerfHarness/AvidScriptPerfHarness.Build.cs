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
            "AvidScriptRuntime",
            "AvidScriptVM",
            "Json",
            "JsonUtilities",
            "OpenSSL",
            "Projects",
            "SSL"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("AvidScriptEditor");
        }

        foreach (string ScriptName in new[] { "reflection.js", "static.js" })
        {
            string ScriptPath = System.IO.Path.Combine(
                PluginDirectory,
                "Content",
                "JavaScript",
                ScriptName);
            if (!System.IO.File.Exists(ScriptPath))
            {
                throw new BuildException($"AvidScript benchmark workload is missing: {ScriptPath}");
            }
            ExternalDependencies.Add(ScriptPath);
            RuntimeDependencies.Add(ScriptPath, StagedFileType.UFS);
        }

        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
    }
}

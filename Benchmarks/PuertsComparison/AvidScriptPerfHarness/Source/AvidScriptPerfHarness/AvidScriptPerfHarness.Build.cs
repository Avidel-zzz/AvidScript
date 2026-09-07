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

        if (Target.ProjectFile == null)
        {
            throw new BuildException("AvidScriptPerfHarness requires a project target to stage Puerts scripts.");
        }
        string PuertsRuntimeRoot = System.IO.Path.Combine(
            Target.ProjectFile.Directory.FullName,
            "Plugins",
            "Puerts",
            "Content",
            "JavaScript",
            "puerts");
        if (!System.IO.Directory.Exists(PuertsRuntimeRoot))
        {
            throw new BuildException($"Puerts runtime script root is missing: {PuertsRuntimeRoot}");
        }
        string[] PuertsRuntimeScripts = System.IO.Directory.GetFiles(
            PuertsRuntimeRoot,
            "*.js",
            System.IO.SearchOption.TopDirectoryOnly);
        System.Array.Sort(PuertsRuntimeScripts, System.StringComparer.Ordinal);
        foreach (string ScriptPath in PuertsRuntimeScripts)
        {
            ExternalDependencies.Add(ScriptPath);
            RuntimeDependencies.Add(ScriptPath, StagedFileType.UFS);
        }

        AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
    }
}

using System;
using System.IO;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticCliTests
{
    public static int Run()
    {
        SuccessfulCliWritesSemanticArtifact();
        SemanticFailureStillWritesArtifact();
        ReferenceSourceBindsExternalFacade();
        ExecutableReferenceSourceProjectsFacade();
        MalformedFrontendReturnsArtifactError();
        InvalidArgumentsReturnUsageExitCode();
        return 6;
    }

    private static void SuccessfulCliWritesSemanticArtifact()
    {
        const string source = "namespace Game; class Script { float Speed; void Tick(float dt) { Speed = dt; } }";
        using TempSemanticWorkspace workspace = TempSemanticWorkspace.Create(source, "Scripts/Script.cs");

        int exitCode = SemanticCommandLine.Run(workspace.CreateArguments());

        Assert(exitCode == 0, "valid semantic CLI input should return 0");
        Assert(File.Exists(workspace.OutputPath), "semantic CLI should write its output artifact");
        using JsonDocument json = JsonDocument.Parse(File.ReadAllBytes(workspace.OutputPath));
        Assert(json.RootElement.GetProperty("succeeded").GetBoolean(), "semantic CLI artifact should report success");
        Assert(json.RootElement.GetProperty("symbols").GetArrayLength() > 0, "semantic CLI artifact should contain symbols");
        Assert(json.RootElement.GetProperty("callables").GetArrayLength() > 0, "semantic CLI artifact should contain callables");
    }

    private static void SemanticFailureStillWritesArtifact()
    {
        const string source = "class Script { void Tick() { int value = \"bad\"; } }";
        using TempSemanticWorkspace workspace = TempSemanticWorkspace.Create(source, "Scripts/Broken.cs");

        int exitCode = SemanticCommandLine.Run(workspace.CreateArguments());

        Assert(exitCode == 1, "semantic errors should return 1");
        Assert(File.Exists(workspace.OutputPath), "semantic errors should still write an artifact");
        string json = File.ReadAllText(workspace.OutputPath);
        Assert(json.Contains("CS0029", StringComparison.Ordinal), "semantic error artifact should retain compiler diagnostics");
    }

    private static void ReferenceSourceBindsExternalFacade()
    {
        const string source = "using AvidScript; public static class CustomScript { public static void Tick() { Actor.SetLocation(1.0f, 2.0f, 3.0f); } }";
        const string referenceSource = "namespace AvidScript { public static class Actor { public static bool SetLocation(float x, float y, float z) => true; } }";
        using TempSemanticWorkspace workspace = TempSemanticWorkspace.Create(source, "Scripts/CustomScript.cs");
        string referencePath = Path.Combine(workspace.Root, "AvidScript.Api.cs");
        File.WriteAllText(referencePath, referenceSource);
        string[] baseArguments = workspace.CreateArguments();
        string[] arguments = new string[baseArguments.Length + 2];
        Array.Copy(baseArguments, arguments, baseArguments.Length);
        arguments[^2] = "--reference-source";
        arguments[^1] = referencePath;

        int exitCode = SemanticCommandLine.Run(arguments);

        Assert(exitCode == 0, "reference source should make the external facade available to semantic analysis");
        string json = File.ReadAllText(workspace.OutputPath);
        Assert(json.Contains("global::AvidScript.Actor.SetLocation(float32,float32,float32):bool", StringComparison.Ordinal),
            "reference source invocation should bind its stable facade symbol");
    }

    private static void ExecutableReferenceSourceProjectsFacade()
    {
        const string source = """
using System.Runtime.InteropServices;
namespace AvidScript;
public static class CustomScript
{
    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay() => Actor.SetScale(3.0f);
}
""";
        const string referenceSource = """
using System.Runtime.InteropServices;
namespace AvidScript;
public static class Actor
{
    public static void SetScale(float value) => Native.SetScale(value);
}
internal static class Native
{
    [DllImport("avidscript", EntryPoint = "avid_ue_cli_scale")]
    internal static extern void SetScale(float value);
}
""";
        using TempSemanticWorkspace workspace = TempSemanticWorkspace.Create(
            source,
            "Scripts/ExecutableCustomScript.cs");
        string referencePath = Path.Combine(workspace.Root, "AvidScript.Bindings.generated.cs");
        File.WriteAllText(referencePath, referenceSource);
        string[] baseArguments = workspace.CreateArguments();
        string[] arguments = new string[baseArguments.Length + 2];
        Array.Copy(baseArguments, arguments, baseArguments.Length);
        arguments[^2] = "--executable-reference-source";
        arguments[^1] = referencePath;

        int exitCode = SemanticCommandLine.Run(arguments);

        Assert(exitCode == 0, "executable reference source should pass the semantic CLI");
        string json = File.ReadAllText(workspace.OutputPath);
        Assert(json.Contains("avid_ue_cli_scale", StringComparison.Ordinal),
            "executable reference DllImport should be serialized");
        Assert(json.Contains("global::AvidScript.Actor.SetScale(float32):void", StringComparison.Ordinal),
            "executable facade wrapper should be serialized as a callable");
    }
    private static void MalformedFrontendReturnsArtifactError()
    {
        using TempSemanticWorkspace workspace = TempSemanticWorkspace.Create("class Script { }", "Scripts/Malformed.cs");
        File.WriteAllText(workspace.FrontendPath, "{}");

        int exitCode = RunSilently(workspace.CreateArguments());

        Assert(exitCode == 2, "malformed frontend artifacts should return 2");
        Assert(!File.Exists(workspace.OutputPath), "malformed frontend artifacts should not create semantic output");
    }

    private static void InvalidArgumentsReturnUsageExitCode()
    {
        Assert(RunSilently(Array.Empty<string>()) == 2, "missing CLI arguments should return 2");
    }

    private static int RunSilently(string[] args)
    {
        TextWriter originalError = Console.Error;
        using StringWriter capturedError = new();
        try
        {
            Console.SetError(capturedError);
            return SemanticCommandLine.Run(args);
        }
        finally
        {
            Console.SetError(originalError);
        }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed class TempSemanticWorkspace : IDisposable
    {
        private TempSemanticWorkspace(string root, string sourcePath, string frontendPath, string outputPath, string sourceId)
        {
            Root = root;
            SourcePath = sourcePath;
            FrontendPath = frontendPath;
            OutputPath = outputPath;
            SourceId = sourceId;
        }

        public string Root { get; }

        public string SourcePath { get; }

        public string FrontendPath { get; }

        public string OutputPath { get; }

        public string SourceId { get; }

        public static TempSemanticWorkspace Create(string source, string sourceId)
        {
            string root = Path.Combine(Path.GetTempPath(), "AvidScriptSemanticTests", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            string sourcePath = Path.Combine(root, "input.cs");
            string frontendPath = Path.Combine(root, "input.frontend.json");
            string outputPath = Path.Combine(root, "input.semantic.json");
            File.WriteAllText(sourcePath, source);
            FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
            File.WriteAllBytes(frontendPath, FrontendSerializer.Serialize(frontend));
            return new TempSemanticWorkspace(root, sourcePath, frontendPath, outputPath, sourceId);
        }

        public string[] CreateArguments()
        {
            return new[]
            {
                "--source", SourcePath,
                "--source-id", SourceId,
                "--frontend", FrontendPath,
                "--output", OutputPath,
            };
        }

        public void Dispose()
        {
            Directory.Delete(Root, recursive: true);
        }
    }
}

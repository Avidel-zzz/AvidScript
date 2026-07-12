using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using AvidScript.CSharpFrontend;

internal static class Program
{
    private static int Main()
    {
        try
        {
            ValidSourcePreservesTokensAndTrivia();
            InvalidSourceReportsPreciseSpan();
            SerializationIsDeterministic();
            CliWritesDeterministicArtifactsAndExitCodes();
            Console.WriteLine("AvidScript.CSharpFrontend.Tests: 4/4 passed");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception);
            return 1;
        }
    }

    private static void ValidSourcePreservesTokensAndTrivia()
    {
        const string source = "// lifecycle\npublic sealed class Script { void Tick(float dt) { value += dt; } }\n";
        FrontendDocument document = FrontendAnalyzer.Analyze(source, "Scripts/Script.cs");

        Assert(document.Succeeded, "valid source should succeed");
        Assert(document.Source.SourceId == "Scripts/Script.cs", "source id should be preserved");
        Assert(document.Syntax.Kind == "CompilationUnit", "syntax root kind should be stable");
        Assert(document.Syntax.Span.Start == 0 && document.Syntax.Span.Length == source.Length, "syntax root should cover the complete source including trivia");
        Assert(document.Tokens.Any(token => token.Kind == "ClassKeyword" && token.Text == "class"), "class token should be present");
        Assert(document.Trivia.Any(trivia => trivia.Kind == "SingleLineCommentTrivia" && trivia.Text == "// lifecycle"), "comment trivia should be preserved");
    }

    private static void InvalidSourceReportsPreciseSpan()
    {
        const string source = "class Script\n{\n    void Tick(\n}\n";
        FrontendDocument document = FrontendAnalyzer.Analyze(source, "Scripts/Broken.cs");
        FrontendDiagnostic diagnostic = document.Diagnostics.First(item => item.Severity == "error");

        Assert(!document.Succeeded, "invalid source should fail");
        Assert(diagnostic.Code == "CS1002", "first Roslyn diagnostic code should be stable");
        Assert(diagnostic.Span.Start == 29 && diagnostic.Span.Length == 0 && diagnostic.Span.End == 29, "diagnostic offsets should identify the missing token");
        Assert(diagnostic.Span.Line == 2 && diagnostic.Span.Column == 14, "diagnostic start should use precise zero-based coordinates");
        Assert(diagnostic.Span.EndLine == 2 && diagnostic.Span.EndColumn == 14, "diagnostic end should use precise zero-based coordinates");
    }

    private static void SerializationIsDeterministic()
    {
        const string source = "public class Script { private static float Time = 1.0f; }\n";
        FrontendDocument firstDocument = FrontendAnalyzer.Analyze(source, @"Scripts\Deterministic.cs");
        FrontendDocument secondDocument = FrontendAnalyzer.Analyze(source, @"Scripts\Deterministic.cs");

        byte[] first = FrontendSerializer.Serialize(firstDocument);
        byte[] second = FrontendSerializer.Serialize(secondDocument);

        Assert(first.SequenceEqual(second), "serialization should be byte deterministic");
        Assert(first.Length > 0 && first[^1] == (byte)'\n', "serialized artifact should use an LF terminator");
        Assert(firstDocument.Source.SourceId == "Scripts/Deterministic.cs", "source id should normalize Windows separators");
    }

    private static void CliWritesDeterministicArtifactsAndExitCodes()
    {
        string caseDirectory = Path.Combine(Directory.GetCurrentDirectory(), "Saved", "AvidScriptFrontendDotNet", "PermanentCliTest");
        Directory.CreateDirectory(caseDirectory);
        string sourcePath = Path.Combine(caseDirectory, "Valid.cs");
        string invalidSourcePath = Path.Combine(caseDirectory, "Invalid.cs");
        string firstOutputPath = Path.Combine(caseDirectory, "First.json");
        string secondOutputPath = Path.Combine(caseDirectory, "Second.json");
        string invalidOutputPath = Path.Combine(caseDirectory, "Invalid.json");
        File.WriteAllText(sourcePath, "// cli\npublic class Script { void Tick(float dt) { } }\n", new UTF8Encoding(false));
        File.WriteAllText(invalidSourcePath, "class Script { void Tick( }\n", new UTF8Encoding(false));
        File.WriteAllText(firstOutputPath, "stale", new UTF8Encoding(false));

        Assert(RunFrontendCli(Array.Empty<string>()) == 2, "missing CLI arguments should return 2");
        Assert(RunFrontendCli(new[] { "--source", sourcePath, "--source-id", @"Scripts\Valid.cs", "--output", firstOutputPath }) == 0, "valid CLI input should return 0 and replace an existing artifact");
        Assert(RunFrontendCli(new[] { "--source", sourcePath, "--source-id", @"Scripts\Valid.cs", "--output", secondOutputPath }) == 0, "repeated valid CLI input should return 0");
        Assert(RunFrontendCli(new[] { "--source", invalidSourcePath, "--source-id", "Scripts/Invalid.cs", "--output", invalidOutputPath }) == 1, "syntax errors should return 1 while writing diagnostics");
        string concurrentOutputPath = Path.Combine(caseDirectory, "Concurrent.json");
        Process[] concurrentProcesses = Enumerable.Range(0, 8)
            .Select(_ => StartFrontendCli(new[] { "--source", sourcePath, "--source-id", "Scripts/Concurrent.cs", "--output", concurrentOutputPath }))
            .ToArray();
        try
        {
            foreach (Process process in concurrentProcesses)
            {
                process.WaitForExit();
                Assert(process.ExitCode == 0, "concurrent atomic writers should all succeed");
            }
        }
        finally
        {
            foreach (Process process in concurrentProcesses)
            {
                process.Dispose();
            }
        }

        Assert(File.ReadAllBytes(firstOutputPath).SequenceEqual(File.ReadAllBytes(secondOutputPath)), "independent CLI processes should emit identical bytes");
        using JsonDocument firstJson = JsonDocument.Parse(File.ReadAllBytes(firstOutputPath));
        using JsonDocument invalidJson = JsonDocument.Parse(File.ReadAllBytes(invalidOutputPath));
        Assert(firstJson.RootElement.GetProperty("source").GetProperty("source_id").GetString() == "Scripts/Valid.cs", "CLI artifact should normalize source id");
        Assert(invalidJson.RootElement.GetProperty("succeeded").GetBoolean() == false, "syntax error artifact should report failure");
        Assert(invalidJson.RootElement.GetProperty("diagnostics").GetArrayLength() > 0, "syntax error artifact should retain diagnostics");
        Assert(!Directory.EnumerateFiles(caseDirectory, "*.tmp", SearchOption.TopDirectoryOnly).Any(), "atomic writes should clean temporary files");
    }

    private static int RunFrontendCli(string[] arguments)
    {
        using Process process = StartFrontendCli(arguments);
        process.WaitForExit();
        return process.ExitCode;
    }

    private static Process StartFrontendCli(string[] arguments)
    {
        string assemblyDirectory = Path.GetDirectoryName(typeof(FrontendAnalyzer).Assembly.Location)
            ?? throw new InvalidOperationException("frontend assembly directory is unavailable");
        string executableName = OperatingSystem.IsWindows()
            ? "AvidScript.CSharpFrontend.exe"
            : "AvidScript.CSharpFrontend";
        ProcessStartInfo startInfo = new(Path.Combine(assemblyDirectory, executableName))
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        foreach (string argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        return Process.Start(startInfo)
            ?? throw new InvalidOperationException("frontend CLI process could not be started");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

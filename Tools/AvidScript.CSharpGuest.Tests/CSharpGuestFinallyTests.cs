using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestFinallyTests
{
    public static int Run()
    {
        NormalAndAbruptCleanupCompiles();
        CatchAndThrowRemainRejected();
        return 2;
    }

    private static void NormalAndAbruptCleanupCompiles()
    {
        string source = File.ReadAllText(FindFixture());
        SemanticDocument semantic = Analyze(source);
        Check(semantic.Succeeded,
            "try/finally should project: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        CSharpGuestLoweringResult guest = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
        Check(guest.Succeeded,
            "try/finally should lower: " + string.Join(" | ", guest.Diagnostics.Select(item => item.Message)));
        Check(!CSharpGuestLowerer.Lower(
            semantic with { SemanticVersion = "1.38" }, new string('a', 64)).Succeeded,
            "structured cleanup cannot be published under the previous semantic contract");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(guest.Module!);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "try/finally should compile to nonempty WASM");
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_FINALLY_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "finally-cleanup.wasm"), wasm.Bytes);
        }
    }

    private static void CatchAndThrowRemainRejected()
    {
        const string source = """
            public static class Script
            {
                public static int Catch()
                {
                    try { throw new System.Exception(); }
                    catch { return 1; }
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(!semantic.Succeeded && semantic.ControlFlowGraphs.Count == 0,
            "language exceptions and catch must remain fail-closed until propagation is implemented");
    }

    private static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/FinallyCleanup.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
    }

    private static string FindFixture()
    {
        for (DirectoryInfo? directory = new(AppContext.BaseDirectory);
            directory is not null; directory = directory.Parent)
        {
            string candidate = Path.Combine(directory.FullName, "Fixtures", "Phase66", "FinallyCleanup.cs");
            if (File.Exists(candidate)) return candidate;
        }
        throw new FileNotFoundException("Fixtures/Phase66/FinallyCleanup.cs");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

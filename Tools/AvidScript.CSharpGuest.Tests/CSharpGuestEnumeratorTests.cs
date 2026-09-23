using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestEnumeratorTests
{
    public static int Run()
    {
        string source = FindFixture();
        const string sourceId = "Scripts/EnumeratorCleanup.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Check(semantic.Succeeded,
            "sealed source enumerator should project: "
            + string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        Check(!CSharpGuestLowerer.Lower(
            semantic with { SemanticVersion = "1.39" }, new string('a', 64)).Succeeded,
            "an enumerator cleanup plan cannot be downgraded to semantic 1.39");
        CSharpGuestLoweringResult guest = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
        Check(guest.Succeeded,
            "enumerator cleanup should lower: "
            + string.Join(" | ", guest.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(guest.Module!);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "enumerator cleanup should compile to nonempty WASM: "
            + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ENUMERATOR_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "enumerator-cleanup.wasm"), wasm.Bytes);
        }
        return 1;
    }

    private static string FindFixture()
    {
        for (DirectoryInfo? directory = new(AppContext.BaseDirectory);
            directory is not null; directory = directory.Parent)
        {
            string path = Path.Combine(directory.FullName, "Fixtures", "Phase66", "EnumeratorCleanup.cs");
            if (File.Exists(path)) return File.ReadAllText(path);
        }
        throw new FileNotFoundException("Fixtures/Phase66/EnumeratorCleanup.cs");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

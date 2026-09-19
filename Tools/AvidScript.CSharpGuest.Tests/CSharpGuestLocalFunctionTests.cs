using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestLocalFunctionTests
{
    public static int Run()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run(int value)
                {
                    static int Sum(int n)
                    {
                        static int Twice(int x) => x * 2;
                        return n <= 0 ? 0 : Sum(n - 1) + Twice(n);
                    }
                    static void Adjust(ref int target, out int previous, int delta = 7)
                    {
                        previous = target;
                        target += delta;
                    }
                    int result = Sum(value);
                    Adjust(ref result, out int before);
                    return result + before;
                }
                [UnmanagedCallersOnly(EntryPoint = "branch")]
                public static int Branch(int value)
                {
                    if (value > 0) { static int Same() => 11; return Same(); }
                    else { static int Same() => 29; return Same(); }
                }
                private static int Unused()
                {
                    static int Same() => 999;
                    return Same();
                }
            }
            """;
        const string sourceId = "Scripts/LocalFunctionsExecution.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Require(semantic.Succeeded, string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowered.Module!;
        Require(GuestModuleValidator.Validate(module).Succeeded,
            "lexical calls and ref/out storage must pass Guest IR validation");
        Require(module.Functions.Count == 7 && module.Imports.Count == 0,
            "only the reachable lexical functions should enter the module, without synthetic host imports");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded && wasm.Bytes.Length > 0, "local function module must compile to WASM");
        WasmCompilationResult repeated = WasmModuleCompiler.Compile(module);
        Require(wasm.Bytes.SequenceEqual(repeated.Bytes), "WASM bytes must be deterministic");
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_LOCAL_FUNCTION_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "local-functions.wasm"), wasm.Bytes);
        }
        foreach (string body in new[]
        {
            "static int Local(int n) => n * 2; Score = Local(3); await AvidContinuations.NextTickAsync(); Score = Local(4);",
            "if (Score == 0) { static int Local(int n) => n * 2; Score = Local(3); } await AvidContinuations.NextTickAsync();",
            "if (Score == 0) { static int Local(int n) => n * 2; await AvidContinuations.NextTickAsync(); Score = Local(4); }",
        })
        {
            string asyncSource = "using System.Runtime.InteropServices; using AvidScript; public static class Script { "
                + "public static int Score; [UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] "
                + "public static async void BeginPlay() { " + body + " } }";
            SemanticDocument asyncSemantic = CSharpGuestContinuationTests.Analyze(asyncSource, "Scripts/AsyncLocalFunctions.cs");
            CSharpGuestLoweringResult asyncLowered = CSharpGuestLowerer.Lower(asyncSemantic, new string('b', 64));
            Require(asyncLowered.Succeeded,
                string.Join(" | ", asyncLowered.Diagnostics.Select(item => item.Message)));
            Require(WasmModuleCompiler.Compile(asyncLowered.Module!).Succeeded,
                "static local helpers in direct, structured and resumable async flow must compile to WASM");
        }
        return 5;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

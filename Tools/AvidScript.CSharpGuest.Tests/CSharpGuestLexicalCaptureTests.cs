using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestLexicalCaptureTests
{
    public static int Run()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run(int seed)
                {
                    int shared = seed;
                    void Add(int amount) { shared += amount; }
                    int Read() => shared;
                    int Alias(ref int value) { shared += 1; value *= 2; return Read(); }
                    int Recurse(int n) { if (n <= 0) return Read(); Add(n); return Recurse(n - 1); }
                    int Outer()
                    {
                        int offset = 5;
                        int Inner() { shared += offset; return Recurse(3); }
                        return Inner();
                    }
                    Add(2);
                    int aliased = Alias(ref shared);
                    int nested = Outer();
                    return aliased * 1000 + nested;
                }
                [UnmanagedCallersOnly(EntryPoint = "parameter")]
                public static int Parameter(int value)
                {
                    int Change() { value += 5; return value; }
                    return Change() + value;
                }
                [UnmanagedCallersOnly(EntryPoint = "loop")]
                public static int Loop(int count)
                {
                    int total = 0;
                    for (int i = 0; i < count; ++i)
                    {
                        int Observe() => i;
                        void Add() { total += Observe(); }
                        Add();
                    }
                    return total;
                }
                [UnmanagedCallersOnly(EntryPoint = "constant")]
                public static int Constant()
                {
                    const int offset = 37;
                    static int ReadConstant() => offset;
                    return ReadConstant();
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        SemanticCallable alias = semantic.Callables.Single(item => item.MethodSymbolId.Contains(":local:Alias("));
        Require(alias.IsStatic && alias.Parameters.Count == 2
            && alias.Parameters.All(parameter => parameter.RefKind == "ref"),
            "explicit and captured arguments must share ref semantics");
        GuestModule module = Lower(semantic);
        Require(module.Imports.Count == 0, "direct captures must not introduce heap or host dependencies");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded, "capturing local calls must compile to WASM");
        SemanticDocument repeated = Analyze(source);
        Require(SemanticSerializer.Serialize(semantic).SequenceEqual(SemanticSerializer.Serialize(repeated)),
            "capture fixed-point normalization must be deterministic");
        Require(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(Lower(repeated)).Bytes),
            "capture lowering must produce deterministic WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_LOCAL_FUNCTION_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "lexical-captures.wasm"), wasm.Bytes);
        }
        foreach (bool controlFlow in new[] { false, true })
        {
            string body = "int captured = Seed; int Read() => captured; void Add() { captured += 4; } "
                + (controlFlow ? "if (captured > 0) { " : "")
                + "await AvidContinuations.NextTickAsync(); Add(); Result = Read(); "
                + "await AvidContinuations.NextTickAsync(); Add(); Result = Read(); "
                + (controlFlow ? "}" : "");
            string asyncSource = "using System.Runtime.InteropServices; using AvidScript; public static class Script { "
                + "public static int Seed; public static int Result; "
                + "[UnmanagedCallersOnly(EntryPoint = \"set_seed\")] public static void SetSeed(int value) { Seed = value; } "
                + "[UnmanagedCallersOnly(EntryPoint = \"result\")] public static int ReadResult() => Result; "
                + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static async void BeginPlay() { "
                + body + " } }";
            SemanticDocument asyncSemantic = CSharpGuestContinuationTests.Analyze(asyncSource, "Scripts/AsyncCaptures.cs");
            SemanticSymbol captured = asyncSemantic.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == "captured");
            Require(asyncSemantic.AsyncMethods.Single().Segments.Where(segment => segment.AwaitSite is not null)
                .All(segment => segment.AwaitSite!.StateFrame!.Slots.Any(slot => slot.SymbolId == captured.Id)),
                "indirect captures used after await must remain live in every outgoing frame");
            GuestModule asyncModule = Lower(asyncSemantic);
            WasmCompilationResult asyncWasm = WasmModuleCompiler.Compile(asyncModule);
            Require(asyncWasm.Succeeded, "capturing helpers must compile across sequential and CFG awaits");
            if (!string.IsNullOrWhiteSpace(output))
            {
                File.WriteAllBytes(Path.Combine(output, controlFlow ? "async-captures-cfg.wasm" : "async-captures-direct.wasm"), asyncWasm.Bytes);
                File.WriteAllBytes(Path.Combine(output, controlFlow ? "async-captures-cfg.guest.json" : "async-captures-direct.guest.json"), GuestIrSerializer.Serialize(asyncModule));
            }
        }
        return 5;
    }

    internal static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/LexicalCaptures.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Require(semantic.Succeeded, string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        return semantic;
    }

    private static GuestModule Lower(SemanticDocument semantic)
    {
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(semantic, new string('c', 64));
        Require(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
        Require(GuestModuleValidator.Validate(result.Module!).Succeeded, "captured ref calls must validate");
        return result.Module!;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

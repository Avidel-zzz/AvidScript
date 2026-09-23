using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestLanguageOutcomeTests
{
    public static int Run()
    {
        DirectCallerChecksOutcomeAndPropagatesError();
        DifferentReturnTypesCopyErrorFieldsAndRoot();
        MultipleCallsAndVoidResultsKeepTheirChecks();
        IncompleteEffectClosureIsRejected();
        CleanupCallerIsRejectedUntilItHasErrorEdges();
        return 5;
    }

    private static void DirectCallerChecksOutcomeAndPropagatesError()
    {
        (SemanticDocument semantic, GuestModule original) = LowerNormalModule();
        string leaf = original.Functions.Single(function => function.Id.Contains(".Leaf(", StringComparison.Ordinal)).Id;
        string wrap = original.Functions.Single(function => function.Id.Contains(".Wrap(", StringComparison.Ordinal)).Id;
        Check(CSharpLanguageOutcomeRewriter.TryRewrite(semantic, original,
                new[] { leaf, wrap }.ToHashSet(StringComparer.Ordinal),
                out GuestModule? rewritten, out string? error)
            && rewritten is not null, error ?? "outcome rewrite failed");
        Check(rewritten!.SchemaVersion == 16 && rewritten.IrVersion == "1.15"
            && rewritten.LanguageOutcomeTypes is { Count: 1 }
            && rewritten.Functions.Where(function => function.Id == leaf || function.Id == wrap)
                .All(function => function.ReturnTypeId == rewritten.LanguageOutcomeTypes[0].TypeId),
            "affected ordinary functions must return the same versioned outcome type");
        GuestFunction caller = rewritten.Functions.Single(function => function.Id == wrap);
        Check(caller.Blocks.Any(block => block.Terminator.Kind == "branch_if"
                && block.Instructions.TakeLast(2).Select(instruction => instruction.Op)
                    .SequenceEqual(new[] { "call", "field_load" }))
            && caller.Blocks.Any(block => block.Terminator.Kind == "return"
                && block.Instructions.Count == 0),
            "each affected call must check status immediately and return the error outcome");
        GuestValidationResult validation = GuestModuleValidator.Validate(rewritten);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(rewritten);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "rewritten C# functions must compile to WASM");
        string? wasmDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_LANGUAGE_OUTCOME_REWRITE_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(wasmDirectory))
        {
            Directory.CreateDirectory(wasmDirectory);
            GuestModule normalProbe = AddProbe(rewritten, wrap, errorProducer: false, producerId: leaf);
            GuestModule errorProbe = AddProbe(rewritten, wrap, errorProducer: true, producerId: leaf);
            WasmCompilationResult normalWasm = WasmModuleCompiler.Compile(normalProbe);
            WasmCompilationResult errorWasm = WasmModuleCompiler.Compile(errorProbe);
            Check(normalWasm.Succeeded && errorWasm.Succeeded,
                "both rewritten outcome paths must compile to WASM");
            File.WriteAllBytes(Path.Combine(wasmDirectory, "outcome-normal.wasm"), normalWasm.Bytes);
            File.WriteAllBytes(Path.Combine(wasmDirectory, "outcome-error.wasm"), errorWasm.Bytes);
        }
    }

    private static void IncompleteEffectClosureIsRejected()
    {
        (SemanticDocument semantic, GuestModule original) = LowerNormalModule();
        string leaf = original.Functions.Single(function => function.Id.Contains(".Leaf(", StringComparison.Ordinal)).Id;
        Check(!CSharpLanguageOutcomeRewriter.TryRewrite(semantic, original,
                new[] { leaf }.ToHashSet(StringComparer.Ordinal), out _, out string? error)
            && error is not null && error.Contains("missing", StringComparison.Ordinal),
            "an unconverted direct caller must not silently consume an outcome as a normal value");
        string wrap = original.Functions.Single(function => function.Id.Contains(".Wrap(", StringComparison.Ordinal)).Id;
        IReadOnlySet<string> complete = new[] { leaf, wrap }.ToHashSet(StringComparer.Ordinal);
        Check(!CSharpLanguageOutcomeRewriter.TryRewrite(semantic with
            {
                Source = semantic.Source with { Sha256 = new string('0', 64) },
            }, original, complete, out _, out string? provenanceError)
            && provenanceError is not null && provenanceError.Contains("provenance", StringComparison.Ordinal),
            "an unrelated Semantic artifact cannot waive cleanup checks for this module");
        Check(!CSharpLanguageOutcomeRewriter.TryRewrite(semantic,
                original with { Exports = new[] { new GuestExport("raw", wrap) } },
                complete, out _, out string? exportError)
            && exportError is not null && exportError.Contains("adapter", StringComparison.Ordinal),
            "an outcome function cannot be exported before a Host boundary adapter exists");
    }

    private static void DifferentReturnTypesCopyErrorFieldsAndRoot()
    {
        const string source = """
            class Script
            {
                static int Leaf() => 7;
                static bool Wrap() => Leaf() > 0;
            }
            """;
        (SemanticDocument semantic, GuestModule original) = LowerNormalModule(source);
        string leaf = original.Functions.Single(function => function.Id.Contains(".Leaf(", StringComparison.Ordinal)).Id;
        string wrap = original.Functions.Single(function => function.Id.Contains(".Wrap(", StringComparison.Ordinal)).Id;
        Check(CSharpLanguageOutcomeRewriter.TryRewrite(semantic, original,
                new[] { leaf, wrap }.ToHashSet(StringComparer.Ordinal),
                out GuestModule? rewritten, out string? error)
            && rewritten is not null, error ?? "mixed-return outcome rewrite failed");
        GuestFunction caller = rewritten!.Functions.Single(function => function.Id == wrap);
        Check(caller.Blocks.Any(block => block.Instructions.Any(instruction =>
                instruction.Op == "field_load" && instruction.TargetId == "field:error_root")
            && block.Instructions.Any(instruction =>
                instruction.Op == "field_store" && instruction.TargetId == "field:error_root")),
            "different outcome value types must retain the same managed error root");
        Check(WasmModuleCompiler.Compile(rewritten).Succeeded,
            "mixed-return outcome propagation must compile to WASM");
    }

    private static void CleanupCallerIsRejectedUntilItHasErrorEdges()
    {
        const string source = """
            class Script
            {
                static int Count;
                static int Leaf() => 7;
                static int Wrap()
                {
                    try { return Leaf(); }
                    finally { Count++; }
                }
            }
            """;
        (SemanticDocument semantic, GuestModule original) = LowerNormalModule(source);
        string leaf = original.Functions.Single(function => function.Id.Contains(".Leaf(", StringComparison.Ordinal)).Id;
        string wrap = original.Functions.Single(function => function.Id.Contains(".Wrap(", StringComparison.Ordinal)).Id;
        Check(!CSharpLanguageOutcomeRewriter.TryRewrite(semantic, original,
                new[] { leaf, wrap }.ToHashSet(StringComparer.Ordinal), out _, out string? error)
            && error is not null && error.Contains("cleanup", StringComparison.Ordinal),
            "a failed call inside finally-bearing code cannot skip the cleanup path");
    }

    private static void MultipleCallsAndVoidResultsKeepTheirChecks()
    {
        const string source = """
            class Script
            {
                static int Count;
                static void Leaf() { Count++; }
                static int Wrap() { Leaf(); Leaf(); return Count; }
            }
            """;
        (SemanticDocument semantic, GuestModule original) = LowerNormalModule(source);
        string leaf = original.Functions.Single(function => function.Id.Contains(".Leaf(", StringComparison.Ordinal)).Id;
        string wrap = original.Functions.Single(function => function.Id.Contains(".Wrap(", StringComparison.Ordinal)).Id;
        Check(CSharpLanguageOutcomeRewriter.TryRewrite(semantic, original,
                new[] { leaf, wrap }.ToHashSet(StringComparer.Ordinal),
                out GuestModule? rewritten, out string? error)
            && rewritten is not null, error ?? "void outcome rewrite failed");
        GuestFunction caller = rewritten!.Functions.Single(function => function.Id == wrap);
        Check(caller.Blocks.Count(block => block.Terminator.Kind == "branch_if"
                && block.Instructions[^1].TargetId == "field:status") == 2
            && rewritten.LanguageOutcomeTypes is { Count: 2 }
            && rewritten.LanguageOutcomeTypes.Any(type => type.ValueTypeId is null),
            "each of two void calls must have its own status check and void outcome layout");
        Check(WasmModuleCompiler.Compile(rewritten).Succeeded,
            "multiple void outcome calls must compile to WASM");
    }

    private static (SemanticDocument Semantic, GuestModule Module) LowerNormalModule(string? source = null)
    {
        source ??= """
            class Script
            {
                static int Leaf() => 7;
                static int Wrap() => Leaf() + 1;
            }
            """;
        const string sourceId = "Scripts/LanguageOutcomeRewrite.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Check(semantic.Succeeded,
            string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        CSharpGuestLoweringResult guest = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
        Check(guest.Succeeded && guest.Module is not null,
            string.Join(" | ", guest.Diagnostics.Select(item => item.Message)));
        return (semantic, guest.Module!);
    }

    private static GuestModule AddProbe(GuestModule module, string targetId,
        bool errorProducer, string producerId)
    {
        string outcomeId = module.Functions.Single(function => function.Id == targetId).ReturnTypeId;
        GuestFunction probe = new("function:outcome_rewrite_probe",
            Array.Empty<GuestRegister>(), new[]
            {
                new GuestRegister("probe_result", outcomeId),
                new GuestRegister("probe_status", "type:int32"),
                new GuestRegister("probe_value", "type:int32"),
                new GuestRegister("probe_failure", "type:int32"),
            }, "type:int32", "probe_entry", new[]
            {
                new GuestBasicBlock("probe_entry", new[]
                {
                    new GuestInstruction("call", "probe_result", Array.Empty<string>(), targetId, null, null),
                    new GuestInstruction("field_load", "probe_status", new[] { "probe_result" },
                        "field:status", null, null),
                }, new GuestTerminator("branch_if", "probe_status", "probe_error", "probe_success", null)),
                new GuestBasicBlock("probe_error", new[]
                {
                    new GuestInstruction("constant", "probe_failure", Array.Empty<string>(), null, null,
                        new GuestConstant("int32", "99")),
                }, new GuestTerminator("return", null, null, null, "probe_failure")),
                new GuestBasicBlock("probe_success", new[]
                {
                    new GuestInstruction("field_load", "probe_value", new[] { "probe_result" },
                        "field:value", null, null),
                }, new GuestTerminator("return", null, null, null, "probe_value")),
            });
        GuestFunction[] functions = module.Functions.Append(probe).ToArray();
        if (errorProducer)
        {
            GuestFunction leaf = functions.Single(function => function.Id == producerId);
            GuestFunction failure = leaf with
            {
                Locals = new[]
                {
                    new GuestRegister("error_out", leaf.ReturnTypeId),
                    new GuestRegister("error_one", "type:int32"),
                },
                EntryBlockId = "error_entry",
                Blocks = new[]
                {
                    new GuestBasicBlock("error_entry", new GuestInstruction[]
                    {
                        new("stack_alloc", "error_out", Array.Empty<string>(), null, null, null),
                        new("constant", "error_one", Array.Empty<string>(), null, null,
                            new GuestConstant("int32", "1")),
                        new("field_store", null, new[] { "error_out", "error_one" }, "field:status", null, null),
                        new("field_store", null, new[] { "error_out", "error_one" }, "field:error_type", null, null),
                        new("field_store", null, new[] { "error_out", "error_one" }, "field:source", null, null),
                    }, new GuestTerminator("return", null, null, null, "error_out")),
                },
            };
            functions = functions.Select(function => function.Id == producerId ? failure : function).ToArray();
        }
        GuestModule candidate = module with
        {
            Functions = functions,
            Exports = module.Exports.Append(new GuestExport("outcome_rewrite_probe", probe.Id)).ToArray(),
        };
        GuestValidationResult validation = GuestModuleValidator.Validate(candidate);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        return candidate;
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

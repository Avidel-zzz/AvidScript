using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestThrowProducerTests
{
    public static int Run()
    {
        SourceThrowProducesManagedLanguageError();
        SameSourceCallerPropagatesManagedLanguageError();
        ConstructorSideEffectsAreRejected();
        CatchRequiresHandlerLowering();
        return 4;
    }

    private static void SameSourceCallerPropagatesManagedLanguageError()
    {
        const string source = """
            class Script
            {
                static int Fail() { throw new System.Exception(); }
                static int Wrap() => Fail();
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "same-source language error did not lower");
        GuestModule module = compiled!.Module;
        string failId = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal)).Id;
        GuestFunction caller = module.Functions.Single(function =>
            function.Id.Contains(".Wrap(", StringComparison.Ordinal));
        Check(module.SchemaVersion == 17 && module.IrVersion == "1.16"
            && module.Provenance.SemanticSchemaVersion == 34
            && module.Provenance.SemanticVersion == "1.43"
            && module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 } catalog
            && catalog.Types[0].TypeId == "type:global::System.Exception"
            && catalog.Sources[0].SourceId == semantic.Source.SourceId
            && source.Substring(catalog.Sources[0].Start, catalog.Sources[0].Length)
                == "throw new System.Exception();"
            && caller.Blocks.Any(block => block.Instructions.Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == failId)
                && block.Instructions.Any(instruction =>
                    instruction.Op == "field_load" && instruction.TargetId == "field:status")
                && block.Terminator.Kind == "branch_if"),
            "the original exception artifact must produce a checked same-source direct call");
        byte[] serialized = GuestIrSerializer.Serialize(module);
        Check(serialized.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(serialized)))
            && GuestIrSerializer.Deserialize(serialized).LanguageErrorCatalog?.Types[0].TypeId
                == "type:global::System.Exception",
            "the source/type token catalog must round-trip as part of the versioned IR");
        GuestModule probe = AddProbe(module, caller, appendTarget: false);
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "same-source throw and caller must compile together to WASM");
        Check(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(probe).Bytes),
            "language-error metadata must compile deterministically");
        AssertLanguageErrorMetadata(wasm.Bytes, probe);
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "throw-caller.wasm"), wasm.Bytes);
            GuestIrArtifactWriter.Write(Path.Combine(output, "throw-caller.guest.json"), probe);
        }
    }

    private static void SourceThrowProducesManagedLanguageError()
    {
        const string source = "class Script { static int Fail() { throw new System.Exception(); } }";
        SemanticDocument semantic = Analyze(source);
        Check(!semantic.Succeeded && semantic.ExceptionFlows is { Count: 1 }
            && SemanticExceptionFlowContractValidator.IsValid(semantic),
            "real C# throw must yield a validated exception-flow artifact");
        GuestModule host = OutcomeHostModule();
        Check(CSharpThrowProducerLowerer.TryLower(semantic, semantic.ExceptionFlows![0], host,
                out CSharpThrowProducerResult? lowered, out string? error)
            && lowered is not null, error ?? "source throw was not lowered");
        Check(lowered!.Catalog.Types is { Count: 1 } types && types[0].Token == 1
            && types[0].TypeId == "type:global::System.Exception"
            && lowered.Catalog.Sources is { Count: 1 } sites && sites[0].Token == 1
            && source.Substring(sites[0].Span.Start, sites[0].Span.Length)
                == "throw new System.Exception();"
            && lowered.Function.Blocks[0].Instructions.Any(instruction =>
                instruction.Op == "managed_new"),
            "error result must retain its type, source span and managed object root");
        GuestModule probe = AddProbe(host, lowered.Function);
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "source-backed throw producer must compile to WASM");
        Check(WasmArtifactInspector.Inspect(wasm.Bytes).CustomSections.All(section =>
                section.Name != "avidscript.language_errors"),
            "IR 16 must not invent the IR 17 language-error WASM metadata");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "throw-producer.wasm"), wasm.Bytes);
        }
    }

    private static void ConstructorSideEffectsAreRejected()
    {
        const string source = "class Script { static int Fail() { throw new System.Exception(\"message\"); } }";
        SemanticDocument semantic = Analyze(source);
        Check(semantic.ExceptionFlows is { Count: 1 }
            && !CSharpThrowProducerLowerer.TryLower(semantic, semantic.ExceptionFlows[0],
                OutcomeHostModule(), out _, out string? error)
            && error is not null && error.Contains("zero-argument", StringComparison.Ordinal),
            "constructor arguments cannot be dropped while producing a language error");
        Check(!CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out _, out string? compilerError)
            && compilerError is not null && compilerError.Contains("zero-argument", StringComparison.Ordinal),
            "same-source compilation must reject constructor side effects");
    }

    private static void CatchRequiresHandlerLowering()
    {
        const string source = """
            class Script
            {
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception) { return 7; }
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(semantic.ExceptionFlows is { Count: 1 }
            && !CSharpThrowProducerLowerer.TryLower(semantic, semantic.ExceptionFlows[0],
                OutcomeHostModule(), out _, out string? error)
            && error is not null && error.Contains("handler", StringComparison.Ordinal),
            "a catch cannot be bypassed by treating its throw as an uncaught error");
        Check(!CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out _, out string? compilerError)
            && compilerError is not null && compilerError.Contains("handler", StringComparison.Ordinal),
            "same-source compilation must retain the catch boundary");
    }

    private static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/SourceThrow.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
    }

    private static GuestModule OutcomeHostModule()
    {
        const string source = "class Host { static int Normal() => 7; }";
        const string sourceId = "Scripts/OutcomeHost.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        CSharpGuestLoweringResult guest = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
        Check(guest.Succeeded && guest.Module is not null,
            string.Join(" | ", guest.Diagnostics.Select(item => item.Message)));
        string normal = guest.Module!.Functions.Single(function =>
            function.Id.Contains(".Normal(", StringComparison.Ordinal)).Id;
        Check(CSharpLanguageOutcomeRewriter.TryRewrite(semantic, guest.Module,
                new[] { normal }.ToHashSet(StringComparer.Ordinal),
                out GuestModule? rewritten, out string? error)
            && rewritten is not null, error ?? "outcome host module failed");
        return rewritten!;
    }

    private static GuestModule AddProbe(GuestModule module, GuestFunction producer,
        bool appendTarget = true)
    {
        const string probeId = "function:throw_source_probe";
        GuestFunction probe = new(probeId, Array.Empty<GuestRegister>(), new[]
        {
            new GuestRegister("result", producer.ReturnTypeId),
            new GuestRegister("status", "type:int32"),
            new GuestRegister("error_type", "type:int32"),
            new GuestRegister("source", "type:int32"),
            new GuestRegister("root", "type:language_error_root"),
            new GuestRegister("code", "type:int32"),
            new GuestRegister("part", "type:int32"),
            new GuestRegister("total", "type:int32"),
            new GuestRegister("unexpected", "type:int32"),
        }, "type:int32", "entry", new[]
        {
            new GuestBasicBlock("entry", new[]
            {
                new GuestInstruction("call", "result", Array.Empty<string>(), producer.Id, null, null),
                new GuestInstruction("field_load", "status", new[] { "result" }, "field:status", null, null),
            }, new GuestTerminator("branch_if", "status", "error", "success", null)),
            new GuestBasicBlock("error", new GuestInstruction[]
            {
                new("managed_collect", null, Array.Empty<string>(), null, null, null),
                new("field_load", "error_type", new[] { "result" }, "field:error_type", null, null),
                new("field_load", "source", new[] { "result" }, "field:source", null, null),
                new("field_load", "root", new[] { "result" }, "field:error_root", null, null),
                new("managed_get", "code", new[] { "root" }, "field:code", null, null),
                new("binary", "part", new[] { "error_type", "source" }, null, "add", null),
                new("binary", "total", new[] { "part", "code" }, null, "add", null),
            }, new GuestTerminator("return", null, null, null, "total")),
            new GuestBasicBlock("success", new[]
            {
                new GuestInstruction("constant", "unexpected", Array.Empty<string>(), null, null,
                    new GuestConstant("int32", "0")),
            }, new GuestTerminator("return", null, null, null, "unexpected")),
        });
        return module with
        {
            Functions = appendTarget
                ? module.Functions.Append(producer).Append(probe).ToArray()
                : module.Functions.Append(probe).ToArray(),
            Exports = module.Exports.Append(new GuestExport("throw_source_probe", probeId)).ToArray(),
        };
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void AssertLanguageErrorMetadata(byte[] wasm, GuestModule module)
    {
        string[] sections = WasmArtifactInspector.Inspect(wasm).CustomSections
            .Where(section => section.Name == "avidscript.language_errors")
            .Select(section => section.PayloadText)
            .ToArray();
        Check(sections.Length == 1, "IR 17 must publish exactly one language-error WASM section");
        using JsonDocument document = JsonDocument.Parse(sections[0]);
        JsonElement root = document.RootElement;
        GuestLanguageErrorCatalog catalog = module.LanguageErrorCatalog!;
        JsonElement type = root.GetProperty("types")[0];
        JsonElement source = root.GetProperty("sources")[0];
        Check(root.GetProperty("schema_version").GetInt32() == 1
            && root.GetProperty("guest_ir_schema_version").GetInt32() == 17
            && root.GetProperty("guest_ir_version").GetString() == "1.16"
            && root.GetProperty("module_id").GetString() == module.ModuleId
            && root.GetProperty("source_sha256").GetString() == module.Provenance.SourceSha256
            && root.GetProperty("types").GetArrayLength() == catalog.Types.Count
            && root.GetProperty("sources").GetArrayLength() == catalog.Sources.Count
            && type.GetProperty("token").GetInt32() == catalog.Types[0].Token
            && type.GetProperty("type_id").GetString() == catalog.Types[0].TypeId
            && source.GetProperty("token").GetInt32() == catalog.Sources[0].Token
            && source.GetProperty("source_id").GetString() == catalog.Sources[0].SourceId
            && source.GetProperty("source_length").GetInt32() == catalog.Sources[0].SourceLength
            && source.GetProperty("start").GetInt32() == catalog.Sources[0].Start
            && source.GetProperty("length").GetInt32() == catalog.Sources[0].Length
            && source.GetProperty("line").GetInt32() == catalog.Sources[0].Line
            && source.GetProperty("column").GetInt32() == catalog.Sources[0].Column
            && source.GetProperty("end_line").GetInt32() == catalog.Sources[0].EndLine
            && source.GetProperty("end_column").GetInt32() == catalog.Sources[0].EndColumn,
            "WASM metadata must preserve the source-backed type and UTF-16 source span");
    }
}

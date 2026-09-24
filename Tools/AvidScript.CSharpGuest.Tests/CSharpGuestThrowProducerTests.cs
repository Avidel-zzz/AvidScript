using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestThrowProducerTests
{
    public static int Run()
    {
        SourceThrowProducesManagedLanguageError();
        SameSourceCallerPropagatesManagedLanguageError();
        SameSourceCallerCatchesManagedLanguageError();
        MultipleThrowProducersKeepDistinctSourceTokens();
        NonmatchingCatchPropagatesLanguageError();
        ConstructorSideEffectsAreRejected();
        LocalThrowUsesHandlerLowering();
        MultipleLocalThrowSitesKeepDistinctSourceTokens();
        NonmatchingLocalCatchPropagatesLanguageError();
        FinallyRunsBeforeOuterCatch();
        NestedFinallyRunsInnerToOuter();
        LocalThrowRunsFinallyBeforeOuterCatch();
        CleanupThrowReplacesOriginalError();
        CatchRethrowPreservesOriginalError();
        NestedCatchRethrowReachesOuterHandler();
        CatchVariableReadsBoundError();
        return 16;
    }

    private static void MultipleThrowProducersKeepDistinctSourceTokens()
    {
        const string source = """
            using System;
            class Script
            {
                static int FailA() { throw new Exception(); }
                static int FailB() { throw new Exception(); }
                static int CatchA()
                {
                    try { return FailA(); }
                    catch (Exception) { return 11; }
                }
                static int CatchB()
                {
                    try { return FailB(); }
                    catch (Exception) { return 22; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { CatchA(); CatchB(); }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(semantic.ExceptionFlows is { Count: 4 },
            "two throw producers and two catch methods need separate exception flows");
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "multiple throw producers did not lower");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 2 } catalog
            && catalog.Sources[0].Token == 1 && catalog.Sources[1].Token == 2
            && catalog.Sources[0].Start < catalog.Sources[1].Start
            && module.Functions.Count(function => function.Id.Contains(".Fail", StringComparison.Ordinal)) == 2,
            "distinct throw sites must retain deterministic source tokens in one module");
        byte[] serialized = GuestIrSerializer.Serialize(module);
        Check(serialized.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(serialized)))
            && GuestIrSerializer.Deserialize(serialized).LanguageErrorCatalog?.Sources.Count == 2,
            "a multi-source-token module must round-trip canonically");
        SemanticDocument mismatchedLength = semantic with
        {
            ExceptionFlows = semantic.ExceptionFlows!.Select((flow, index) =>
                index == 1 ? flow with { SourceLength = flow.SourceLength + 1 } : flow).ToArray(),
        };
        Check(!CSharpLanguageErrorCompiler.TryLower(mismatchedLength, new string('a', 64),
                out _, out string? mismatchError)
            && mismatchError is not null && mismatchError.Contains("length", StringComparison.Ordinal),
            "conflicting lengths for one source unit must fail closed");
        GuestFunction catchA = module.Functions.Single(function =>
            function.Id.Contains(".CatchA(", StringComparison.Ordinal));
        GuestFunction catchB = module.Functions.Single(function =>
            function.Id.Contains(".CatchB(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(
            AddCatchProbe(module, catchA, "function:catch_source_probe_a", "catch_source_probe_a"),
            catchB, "function:catch_source_probe_b", "catch_source_probe_b");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8
            && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(probe).Bytes),
            "multi-producer catch WASM must compile deterministically");
        AssertLanguageErrorMetadata(wasm.Bytes, probe);
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "multi-catch.wasm"), wasm.Bytes);
        }
    }

    private static void SameSourceCallerCatchesManagedLanguageError()
    {
        const string source = """
            using System;
            class Script
            {
                static int Fail() { throw new Exception(); }
                static int Catch()
                {
                    try { return Fail(); }
                    catch (InvalidOperationException) { return 5; }
                    catch (Exception) { return 7; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Catch(); }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(semantic.ExceptionFlows is { Count: 2 },
            "the source must project separate throw and handler methods");
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null,
            error ?? "source-backed catch failed to lower");
        GuestModule module = compiled!.Module;
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        Check(handler.Blocks.Any(block => block.Instructions.Any(instruction =>
                instruction.Op == "field_load" && instruction.TargetId == "field:error_type")
                && block.Terminator.Kind == "branch_if")
            && GuestModuleValidator.Validate(module).Succeeded,
            "a failed call must select a typed local catch before propagation");
        GuestModule probe = AddCatchProbe(module, handler);
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "the source-backed catch must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-caller.wasm"), wasm.Bytes);
        }
    }

    private static void NonmatchingCatchPropagatesLanguageError()
    {
        const string source = """
            using System;
            class Script
            {
                static int Fail() { throw new Exception(); }
                static int Catch()
                {
                    try { return Fail(); }
                    catch (InvalidOperationException) { return 7; }
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "nonmatching catch failed to lower");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1 } catalog
            && catalog.Types[0].TypeId == "type:global::System.Exception",
            "handler-only types must not become unused runtime error tokens");
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule probe = AddProbe(module, handler, appendTarget: false);
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "an unmatched source catch must propagate the original error in WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-mismatch.wasm"), wasm.Bytes);
        }
    }

    private static void SameSourceCallerPropagatesManagedLanguageError()
    {
        const string source = """
            class Script
            {
                static int Fail() { throw new System.Exception(); }
                static int Wrap() => Fail();
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Wrap(); }
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
        GuestExport entry = module.Exports.Single(export => export.Name == "avid_on_begin_play");
        GuestFunction adapter = module.Functions.Single(function => function.Id == entry.FunctionId);
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
                && block.Terminator.Kind == "branch_if")
            && adapter.ReturnTypeId == "type:void"
            && adapter.Blocks.Any(block => block.Terminator.Kind == "branch_if")
            && adapter.Blocks.Any(block => block.Instructions.Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == "import:language_error_report_v1")
                && block.Terminator.Kind == "trap"),
            "the original exception artifact must produce a checked same-source direct call");
        byte[] serialized = GuestIrSerializer.Serialize(module);
        Check(serialized.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(serialized)))
            && GuestIrSerializer.Deserialize(serialized).LanguageErrorCatalog?.Types[0].TypeId
                == "type:global::System.Exception",
            "the source/type token catalog must round-trip as part of the versioned IR");
        GuestModule probe = AddProbe(module, caller, appendTarget: false);
        GuestImport report = module.Imports.Single(import =>
            import.Name == "avid_language_error_report_v1");
        GuestModule forgedReport = module with
        {
            Imports = module.Imports.Select(import => import == report
                ? import with { Name = "untrusted_language_error_report" } : import).ToArray(),
        };
        Check(!GuestModuleValidator.Validate(forgedReport).Succeeded,
            "arbitrary host imports must not receive module-local managed references");
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
            "same-source compilation must reject constructor side effects: " + compilerError);
    }

    private static void LocalThrowUsesHandlerLowering()
    {
        const string source = """
            class Script
            {
                static int Run()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception) { return 7; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Run(); }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(semantic.ExceptionFlows is { Count: 1 }
            && !CSharpThrowProducerLowerer.TryLower(semantic, semantic.ExceptionFlows[0],
                OutcomeHostModule(), out _, out string? error)
            && error is not null && error.Contains("handler", StringComparison.Ordinal),
            "a catch cannot be bypassed by treating its throw as an uncaught error");
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? compilerError)
            && compiled is not null, compilerError ?? "the local throw did not reach its catch");
        GuestModule module = compiled!.Module;
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        Check(handler.Blocks.Any(block => block.Instructions.Any(instruction =>
                instruction.Op == "managed_new") && block.Terminator.Kind == "branch")
            && module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 }
            && GuestModuleValidator.Validate(module).Succeeded,
            "a local throw must allocate a rooted error and branch to the source-derived catch");
        GuestModule probe = AddCatchProbe(module, handler);
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8
            && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(probe).Bytes),
            "the local catch must compile deterministically to executable WASM");
        AssertLanguageErrorMetadata(wasm.Bytes, probe);
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "local-catch.wasm"), wasm.Bytes);
        }

        const string constructorSource = """
            class Script
            {
                static int Run()
                {
                    try { throw new System.Exception("message"); }
                    catch (System.Exception) { return 7; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(constructorSource), new string('a', 64),
                out _, out string? constructorError)
            && constructorError is not null && constructorError.Contains("zero-argument", StringComparison.Ordinal),
            "a local throw cannot drop constructor arguments");

        const string cleanupSource = """
            class Script
            {
                static int Fail() { throw new System.Exception(); }
                static int Run()
                {
                    try { return Fail(); }
                    finally { throw new System.Exception(); }
                }
            }
            """;
        SemanticDocument cleanupSemantic = Analyze(cleanupSource);
        Check(!CSharpLanguageErrorCompiler.TryLower(cleanupSemantic, new string('a', 64),
                out _, out string? cleanupError)
            && cleanupError is not null && cleanupError.Contains("cleanup", StringComparison.Ordinal),
            "a throwing finally needs error replacement semantics before it can execute");
    }

    private static void NonmatchingLocalCatchPropagatesLanguageError()
    {
        const string source = """
            class Script
            {
                static int Run()
                {
                    try { throw new System.Exception(); }
                    catch (System.InvalidOperationException) { return 7; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Run(); }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "nonmatching local catch did not lower");
        GuestModule module = compiled!.Module;
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        Check(handler.Blocks.Any(block => block.Instructions.Any(instruction =>
                instruction.Op == "managed_new") && block.Terminator.Kind == "return")
            && GuestModuleValidator.Validate(module).Succeeded,
            "an unmatched local catch must return the rooted language error");
        GuestModule probe = AddProbe(module, handler, appendTarget: false);
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "an unmatched local catch must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "local-mismatch.wasm"), wasm.Bytes);
        }
    }

    private static void MultipleLocalThrowSitesKeepDistinctSourceTokens()
    {
        const string source = """
            class Script
            {
                static int CatchA()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception) { return 11; }
                }
                static int CatchB()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception) { return 22; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { CatchA(); CatchB(); }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "multiple local throws did not lower");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 2 } catalog
            && catalog.Sources[0].Start < catalog.Sources[1].Start,
            "local throws must retain separate ordered source positions");
        GuestFunction catchA = module.Functions.Single(function =>
            function.Id.Contains(".CatchA(", StringComparison.Ordinal));
        GuestFunction catchB = module.Functions.Single(function =>
            function.Id.Contains(".CatchB(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(
            AddCatchProbe(module, catchA, "function:catch_source_probe_a", "catch_source_probe_a"),
            catchB, "function:catch_source_probe_b", "catch_source_probe_b");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "multiple local catch methods must compile to WASM");
        AssertLanguageErrorMetadata(wasm.Bytes, probe);
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "multi-local-catch.wasm"), wasm.Bytes);
        }
    }

    private static void FinallyRunsBeforeOuterCatch()
    {
        const string source = """
            class Script
            {
                static int CleanupCount;
                static int Fail() { throw new System.Exception(); }
                static int Run()
                {
                    try { return Fail(); }
                    finally { CleanupCount = CleanupCount + 1; }
                }
                static int Catch()
                {
                    try { return Run(); }
                    catch (System.Exception) { return CleanupCount; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Catch(); }
            }
            """;
        Check(ReferenceCatch(source) == 1,
            "the CLR reference runs finally before the outer catch");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "a language error bypassed finally");
        GuestModule module = compiled!.Module;
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        Check(run.Blocks.Any(block => block.Id.StartsWith("outcome:block:", StringComparison.Ordinal)
                && block.Instructions.Any(instruction => instruction.Op == "global_store"))
            && GuestModuleValidator.Validate(module).Succeeded,
            "the error route must execute a copy of the source cleanup block");
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(module, handler,
            "function:finally_source_probe", "finally_source_probe");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "the error cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "finally-catch.wasm"), wasm.Bytes);
        }

        const string branchingSource = """
            class Script
            {
                static int CleanupCount;
                static int Fail() { throw new System.Exception(); }
                static int Run()
                {
                    try { Fail(); }
                    finally
                    {
                        if (CleanupCount == 0) CleanupCount = 1;
                    }
                    return 0;
                }
            }
            """;
        SemanticDocument branching = Analyze(branchingSource);
        bool branchingLowered = CSharpLanguageErrorCompiler.TryLower(branching, new string('a', 64),
            out _, out string? branchingError);
        Check(!branchingLowered
            && branchingError is not null
            && branchingError.Contains("cleanup", StringComparison.Ordinal),
            $"branching finally must fail closed: lowered={branchingLowered}, error={branchingError}");
    }

    private static void NestedFinallyRunsInnerToOuter()
    {
        const string source = """
            class Script
            {
                static int CleanupCount;
                static int Fail() { throw new System.Exception(); }
                static int Run()
                {
                    try
                    {
                        try { Fail(); }
                        finally { CleanupCount = CleanupCount + 1; }
                    }
                    finally { CleanupCount = CleanupCount + 10; }
                    return 0;
                }
                static int Catch()
                {
                    try { return Run(); }
                    catch (System.Exception) { return CleanupCount; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Catch(); }
            }
            """;
        Check(ReferenceCatch(source) == 11,
            "the CLR reference runs nested finally blocks inside-out");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "nested finally failed to compile");
        GuestFunction run = compiled!.Module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        Check(run.Blocks.Count(block => block.Id.StartsWith("outcome:block:", StringComparison.Ordinal)
                && block.Instructions.Any(instruction => instruction.Op == "global_store")) >= 2
            && GuestModuleValidator.Validate(compiled.Module).Succeeded,
            "the error route must execute the inner then outer cleanup blocks");
        GuestFunction handler = compiled.Module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(compiled.Module, handler,
            "function:nested_finally_source_probe", "nested_finally_source_probe");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "nested error cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "nested-finally-catch.wasm"), wasm.Bytes);
        }
    }

    private static void LocalThrowRunsFinallyBeforeOuterCatch()
    {
        const string source = """
            class Script
            {
                static int CleanupCount;
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    finally { CleanupCount = CleanupCount + 1; }
                }
                static int Catch()
                {
                    try { return Fail(); }
                    catch (System.Exception) { return CleanupCount; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Catch(); }
            }
            """;
        Check(ReferenceCatch(source) == 1,
            "the CLR reference runs a throwing method's finally before its caller catch");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "local throw cleanup failed to compile");
        GuestFunction fail = compiled!.Module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        Check(fail.Blocks.Any(block => block.Instructions.Any(item => item.Op == "global_store"))
            && fail.Blocks.Any(block => block.Instructions.Any(item => item.Op == "managed_new"))
            && GuestModuleValidator.Validate(compiled.Module).Succeeded,
            "the local throw must retain its error root through the cleanup block");
        GuestFunction handler = compiled.Module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(compiled.Module, handler,
            "function:throw_finally_source_probe", "throw_finally_source_probe");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "local throw cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "throw-finally-catch.wasm"), wasm.Bytes);
        }

        const string branchingSource = """
            class Script
            {
                static int Count;
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    finally { if (Count == 0) Count = 1; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(branchingSource), new string('a', 64),
                out _, out string? branchingError)
            && branchingError is not null && branchingError.Contains("cleanup", StringComparison.Ordinal),
            "branching cleanup in a throw method must fail closed");
    }

    private static void CleanupThrowReplacesOriginalError()
    {
        const string source = """
            class Script
            {
                static int Count;
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    finally { Count = Count + 1; throw new System.Exception(); }
                }
                static int Catch()
                {
                    try { return Fail(); }
                    catch (System.Exception) { return Count; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Catch(); }
            }
            """;
        Check(ReferenceCatch(source) == 1,
            "the CLR reference must replace the original error after running cleanup");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "cleanup error replacement failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 2 } catalog
            && source.Substring(catalog.Sources[0].Start, catalog.Sources[0].Length)
                == "throw new System.Exception();"
            && source.Substring(catalog.Sources[1].Start, catalog.Sources[1].Length)
                == "throw new System.Exception();"
            && catalog.Sources[0].Start < catalog.Sources[1].Start,
            "both original and replacing throws must retain source tokens");
        SemanticDocument reordered = semantic with
        {
            ExceptionFlows = semantic.ExceptionFlows!.Select(flow =>
                flow.MethodSymbolId.Contains(".Fail(", StringComparison.Ordinal)
                    ? flow with { Throws = flow.Throws.Reverse().ToArray() } : flow).ToArray(),
        };
        Check(CSharpLanguageErrorCompiler.TryLower(reordered, new string('a', 64),
                out CSharpLanguageErrorCompilation? reorderedCompilation, out string? reorderedError)
            && reorderedCompilation is not null
            && GuestIrSerializer.Serialize(reorderedCompilation.Module)
                .SequenceEqual(GuestIrSerializer.Serialize(module)),
            reorderedError ?? "throw-site catalog order must not change the executable mapping");
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(AddProbe(module, fail, appendTarget: false),
            handler, "function:replacement_catch_probe", "replacement_catch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "cleanup error replacement must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "cleanup-replaces-error.wasm"), wasm.Bytes);
        }

        const string directSource = """
            class Script
            {
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    finally { throw new System.Exception(); }
                }
            }
            """;
        Check(CSharpLanguageErrorCompiler.TryLower(Analyze(directSource), new string('a', 64),
                out CSharpLanguageErrorCompilation? direct, out string? directError)
            && direct is not null && direct.Module.LanguageErrorCatalog?.Sources.Count == 2
            && WasmModuleCompiler.Compile(direct.Module).Succeeded,
            directError ?? "a cleanup-only replacement needs no unrelated side effect");

        const string constructorSource = """
            class Script
            {
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    finally { throw new System.Exception("message"); }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(constructorSource), new string('a', 64),
                out _, out string? constructorError)
            && constructorError is not null && constructorError.Contains("cleanup", StringComparison.Ordinal),
            "a cleanup throw must not drop constructor side effects");
    }

    private static void CatchRethrowPreservesOriginalError()
    {
        const string source = """
            class Script
            {
                static int Throw() { throw new System.Exception(); }
                static int RethrowLocal()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception) { throw; }
                }
                static int RethrowCall()
                {
                    try { return Throw(); }
                    catch (System.Exception) { throw; }
                }
                static int CatchLocal()
                {
                    try { return RethrowLocal(); }
                    catch (System.Exception) { return 7; }
                }
                static int CatchCall()
                {
                    try { return RethrowCall(); }
                    catch (System.Exception) { return 9; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { CatchLocal(); CatchCall(); }
            }
            """;
        Check(ReferenceCatch(source, "CatchLocal") == 7
            && ReferenceCatch(source, "CatchCall") == 9,
            "the CLR reference must route both local and called rethrows to outer handlers");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "catch rethrow failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 2 } catalog
            && catalog.Sources[0].Start < catalog.Sources[1].Start,
            "a rethrow must reuse its original source token without adding a catalog entry");
        GuestFunction local = module.Functions.Single(function =>
            function.Id.Contains(".RethrowLocal(", StringComparison.Ordinal));
        GuestFunction called = module.Functions.Single(function =>
            function.Id.Contains(".RethrowCall(", StringComparison.Ordinal));
        GuestFunction localCatch = module.Functions.Single(function =>
            function.Id.Contains(".CatchLocal(", StringComparison.Ordinal));
        GuestFunction callCatch = module.Functions.Single(function =>
            function.Id.Contains(".CatchCall(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(AddCatchProbe(AddProbe(AddProbe(module, local,
                    appendTarget: false, probeId: "function:rethrow_local_source_probe",
                    exportName: "rethrow_local_source_probe"),
                called, appendTarget: false, probeId: "function:rethrow_call_source_probe",
                exportName: "rethrow_call_source_probe"),
            localCatch, "function:rethrow_local_catch_probe", "rethrow_local_catch_probe"),
            callCatch, "function:rethrow_call_catch_probe", "rethrow_call_catch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "both source-backed rethrow routes must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-rethrow.wasm"), wasm.Bytes);
        }

    }

    private static void NestedCatchRethrowReachesOuterHandler()
    {
        const string source = """
            class Script
            {
                static int Throw() { throw new System.Exception(); }
                static int NestedLocal()
                {
                    try
                    {
                        try { throw new System.Exception(); }
                        catch (System.Exception) { throw; }
                    }
                    catch (System.Exception) { return 3; }
                }
                static int NestedEscape()
                {
                    try
                    {
                        try { return Throw(); }
                        catch (System.Exception) { throw; }
                    }
                    catch (System.Exception) { throw; }
                }
                static int CatchEscape()
                {
                    try { return NestedEscape(); }
                    catch (System.Exception) { return 5; }
                }
                static int NestedMismatch()
                {
                    try
                    {
                        try { return Throw(); }
                        catch (System.Exception) { throw; }
                    }
                    catch (System.InvalidOperationException) { return 1; }
                }
                static int CatchMismatch()
                {
                    try { return NestedMismatch(); }
                    catch (System.Exception) { return 6; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { NestedLocal(); CatchEscape(); CatchMismatch(); }
            }
            """;
        Check(ReferenceCatch(source, "NestedLocal") == 3
            && ReferenceCatch(source, "CatchEscape") == 5
            && ReferenceCatch(source, "CatchMismatch") == 6,
            "the CLR reference must route nested rethrows to their outer handlers");
        Check(CSharpLanguageErrorCompiler.TryLower(Analyze(source), new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "nested catch rethrow failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 2 },
            "nested rethrows must retain only the original throw source tokens");
        GuestFunction local = module.Functions.Single(function =>
            function.Id.Contains(".NestedLocal(", StringComparison.Ordinal));
        GuestFunction escaped = module.Functions.Single(function =>
            function.Id.Contains(".NestedEscape(", StringComparison.Ordinal));
        GuestFunction catchEscape = module.Functions.Single(function =>
            function.Id.Contains(".CatchEscape(", StringComparison.Ordinal));
        GuestFunction catchMismatch = module.Functions.Single(function =>
            function.Id.Contains(".CatchMismatch(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(AddCatchProbe(AddCatchProbe(AddProbe(module, escaped,
                appendTarget: false, probeId: "function:nested_rethrow_source_probe",
                exportName: "nested_rethrow_source_probe"),
            local, "function:nested_rethrow_local_probe", "nested_rethrow_local_probe"),
            catchEscape, "function:nested_rethrow_escape_probe", "nested_rethrow_escape_probe"),
            catchMismatch, "function:nested_rethrow_mismatch_probe", "nested_rethrow_mismatch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "nested catch rethrows must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "nested-rethrow.wasm"), wasm.Bytes);
        }
    }

    private static void CatchVariableReadsBoundError()
    {
        const string source = """
            class Script
            {
                static int Fail() { throw new System.Exception(); }
                static int Catch()
                {
                    try { return Fail(); }
                    catch (System.Exception error) { return error == null ? 0 : 7; }
                }
                static int LocalCatch()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception error) { return error == null ? 0 : 9; }
                }
                static int UnusedCatch()
                {
                    try { return Fail(); }
                    catch (System.Exception unused) { return 11; }
                }
                static int VariableRethrow()
                {
                    try { return Fail(); }
                    catch (System.Exception error) { throw; }
                }
                static int CatchRethrow()
                {
                    try { return VariableRethrow(); }
                    catch (System.Exception) { return 13; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Catch(); LocalCatch(); UnusedCatch(); CatchRethrow(); }
            }
            """;
        Check(ReferenceCatch(source) == 7
            && ReferenceCatch(source, "LocalCatch") == 9
            && ReferenceCatch(source, "UnusedCatch") == 11
            && ReferenceCatch(source, "CatchRethrow") == 13,
            "the CLR reference must observe bound and unused catch variables");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "catch variable failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 2 },
            "catch variable binding must preserve source-backed throw identities");
        GuestFunction caught = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestFunction local = module.Functions.Single(function =>
            function.Id.Contains(".LocalCatch(", StringComparison.Ordinal));
        GuestFunction unused = module.Functions.Single(function =>
            function.Id.Contains(".UnusedCatch(", StringComparison.Ordinal));
        GuestFunction rethrown = module.Functions.Single(function =>
            function.Id.Contains(".CatchRethrow(", StringComparison.Ordinal));
        GuestModule probe = AddCatchProbe(AddCatchProbe(AddCatchProbe(AddCatchProbe(
                AddCatchVariableCollectProbe(module),
                caught, "function:catch_variable_call_probe", "catch_variable_call_probe"),
            local, "function:catch_variable_local_probe", "catch_variable_local_probe"),
            unused, "function:catch_variable_unused_probe", "catch_variable_unused_probe"),
            rethrown, "function:catch_variable_rethrow_probe", "catch_variable_rethrow_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "catch variable binding must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-variable.wasm"), wasm.Bytes);
        }

        SemanticCatchHandler boundHandler = semantic.ExceptionFlows!.Single(flow =>
            flow.MethodSymbolId.Contains(".Catch():", StringComparison.Ordinal)).Catches.Single();
        SemanticDocument missingSymbol = semantic with
        {
            Symbols = semantic.Symbols.Where(symbol =>
                symbol.Id != boundHandler.ExceptionVariableSymbolId).ToArray(),
        };
        Check(!CSharpLanguageErrorCompiler.TryLower(missingSymbol, new string('a', 64),
                out _, out string? missingError)
            && missingError is not null && missingError.Contains("catch variable", StringComparison.Ordinal),
            "a catch variable with no source symbol must fail closed");

        const string subtypeSource = """
            class Script
            {
                static int Fail() { throw new System.Exception(); }
                static int Catch()
                {
                    try { return Fail(); }
                    catch (System.InvalidOperationException error) { return 7; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(subtypeSource), new string('a', 64),
                out _, out string? subtypeError)
            && subtypeError is not null && subtypeError.Contains("catch variable", StringComparison.Ordinal),
            "an unsupported exception variable type must fail closed");

        const string memberSource = """
            class Script
            {
                static int Fail() { throw new System.Exception(); }
                static int Catch()
                {
                    try { return Fail(); }
                    catch (System.Exception error) { return error.Message.Length; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(memberSource), new string('a', 64),
                out _, out string? memberError) && memberError is not null,
            "exception members must stay rejected until their object contract executes");
    }

    private static GuestModule AddCatchVariableCollectProbe(GuestModule module)
    {
        const string exceptionTypeId = "type:global::System.Exception";
        GuestFunction[] functions = module.Functions.Select(function =>
        {
            if (!function.Id.Contains(".Catch(", StringComparison.Ordinal)
                && !function.Id.Contains(".LocalCatch(", StringComparison.Ordinal))
                return function;
            GuestBasicBlock[] bindingBlocks = function.Blocks.Where(block =>
                block.Instructions.Any(instruction => instruction.Op == "managed_cast"
                    && instruction.ResultId is { } result
                    && function.Locals.Any(local => local.Id == result
                        && local.TypeId == exceptionTypeId))).ToArray();
            Check(bindingBlocks.Length == 1, "the catch variable needs one bound handler block");
            GuestBasicBlock binding = bindingBlocks[0];
            GuestInstruction cast = binding.Instructions.Single(instruction =>
                instruction.Op == "managed_cast"
                && instruction.ResultId is { } result
                && function.Locals.Any(local => local.Id == result
                    && local.TypeId == exceptionTypeId));
            int storeIndex = binding.Instructions.ToList().FindIndex(instruction =>
                instruction.Op == "local_store" && instruction.OperandIds.SequenceEqual(
                    new[] { cast.ResultId! }));
            Check(storeIndex >= 0, "the catch variable must receive the captured object");
            string rootId = "catch_variable:collect_root";
            string codeId = "catch_variable:collect_code";
            Check(!function.Locals.Any(local => local.Id is
                    "catch_variable:collect_root" or "catch_variable:collect_code"),
                "the collect probe needs unique registers");
            List<GuestInstruction> instructions = binding.Instructions.ToList();
            instructions.InsertRange(storeIndex + 1, new GuestInstruction[]
            {
                new("managed_collect", null, Array.Empty<string>(), null, null, null),
                new("managed_cast", rootId, new[] { cast.ResultId! }, null, null, null),
                new("managed_get", codeId, new[] { rootId }, "field:code", null, null),
            });
            return function with
            {
                Locals = function.Locals.Concat(new[]
                {
                    new GuestRegister(rootId, "type:language_error_root"),
                    new GuestRegister(codeId, "type:int32"),
                }).ToArray(),
                Blocks = function.Blocks.Select(block => block.Id == binding.Id
                    ? block with { Instructions = instructions } : block).ToArray(),
            };
        }).ToArray();
        return module with { Functions = functions };
    }

    private static int ReferenceCatch(string source, string methodName = "Catch")
    {
        string[] assemblyPaths = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!)
            .Split(Path.PathSeparator);
        CSharpCompilation compilation = CSharpCompilation.Create(
            "AvidScriptLanguageErrorOracle",
            new[] { CSharpSyntaxTree.ParseText(source) },
            assemblyPaths.Select(path => MetadataReference.CreateFromFile(path)),
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: OptimizationLevel.Release));
        using MemoryStream bytes = new();
        var result = compilation.Emit(bytes);
        Check(result.Success,
            string.Join(" | ", result.Diagnostics.Select(diagnostic => diagnostic.ToString())));
        bytes.Position = 0;
        AssemblyLoadContext context = new("avidscript-language-error-oracle", isCollectible: true);
        try
        {
            Type script = context.LoadFromStream(bytes).GetType("Script")!;
            MethodInfo method = script.GetMethod(methodName, BindingFlags.Static | BindingFlags.NonPublic)!;
            return (int)method.Invoke(null, null)!;
        }
        finally
        {
            context.Unload();
        }
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
        bool appendTarget = true,
        string probeId = "function:throw_source_probe",
        string exportName = "throw_source_probe")
    {
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
            Exports = module.Exports.Append(new GuestExport(exportName, probeId)).ToArray(),
        };
    }

    private static GuestModule AddCatchProbe(GuestModule module, GuestFunction handler,
        string probeId = "function:catch_source_probe",
        string exportName = "catch_source_probe")
    {
        GuestFunction probe = new(probeId, Array.Empty<GuestRegister>(), new[]
        {
            new GuestRegister("catch_result", handler.ReturnTypeId),
            new GuestRegister("catch_status", "type:int32"),
            new GuestRegister("catch_value", "type:int32"),
            new GuestRegister("catch_failure", "type:int32"),
        }, "type:int32", "catch_entry", new[]
        {
            new GuestBasicBlock("catch_entry", new[]
            {
                new GuestInstruction("call", "catch_result", Array.Empty<string>(), handler.Id, null, null),
                new GuestInstruction("field_load", "catch_status", new[] { "catch_result" },
                    "field:status", null, null),
            }, new GuestTerminator("branch_if", "catch_status", "catch_error", "catch_success", null)),
            new GuestBasicBlock("catch_error", new[]
            {
                new GuestInstruction("constant", "catch_failure", Array.Empty<string>(), null, null,
                    new GuestConstant("int32", "-1")),
            }, new GuestTerminator("return", null, null, null, "catch_failure")),
            new GuestBasicBlock("catch_success", new[]
            {
                new GuestInstruction("field_load", "catch_value", new[] { "catch_result" },
                    "field:value", null, null),
            }, new GuestTerminator("return", null, null, null, "catch_value")),
        });
        return module with
        {
            Functions = module.Functions.Append(probe).ToArray(),
            Exports = module.Exports.Append(new GuestExport(exportName, probeId)).ToArray(),
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

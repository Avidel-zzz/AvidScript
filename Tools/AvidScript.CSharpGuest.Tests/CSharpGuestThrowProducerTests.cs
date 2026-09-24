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
        NestedLocalThrowRunsFinaliesInnerToOuter();
        MultipleLocalThrowsShareFinallyAndKeepSources();
        NormalReturnsAndLocalThrowsRunTheSameFinally();
        MixedExitsRunBranchingFinally();
        CalledReturnAndLocalThrowRunTheSameFinally();
        CalledReturnRunsBranchingFinally();
        CatchReturnsRunOuterFinally();
        CatchReturnsRunBranchingFinally();
        CleanupThrowReplacesOriginalError();
        NestedCleanupThrowReplacesOriginalError();
        OuterNestedCleanupThrowReplacesOriginalError();
        OutermostCleanupThrowReplacesOriginalError();
        ConsecutiveNestedCleanupThrowsKeepLastError();
        BranchingFinallyRunsBeforeErrorPropagation();
        CatchRethrowRunsOuterFinally();
        CatchRethrowRunsBranchingFinally();
        CatchLocalRethrowRunsOuterFinally();
        CatchLocalRethrowRunsBranchingFinally();
        CatchRethrowPreservesOriginalError();
        NestedCatchRethrowReachesOuterHandler();
        NestedCatchRethrowRunsBranchingOuterFinally();
        CatchVariableReadsBoundError();
        return 34;
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
            out CSharpLanguageErrorCompilation? branchingCompiled, out string? branchingError);
        Check(branchingLowered && branchingCompiled is not null
            && GuestModuleValidator.Validate(branchingCompiled.Module).Succeeded,
            branchingError ?? "single-arm finally must have an executable error route");
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
                static void ChangeCount() { Count = Count + 1; }
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    finally { if (Count == 0) ChangeCount(); }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(branchingSource), new string('a', 64),
                out _, out string? branchingError)
            && branchingError is not null && branchingError.Contains("cleanup", StringComparison.Ordinal),
            "a call inside branching cleanup must fail closed");
    }

    private static void NestedLocalThrowRunsFinaliesInnerToOuter()
    {
        const string source = """
            class Script
            {
                static int CleanupCount;
                static int Fail()
                {
                    try
                    {
                        try { throw new System.Exception(); }
                        finally { CleanupCount = CleanupCount + 1; }
                    }
                    finally { CleanupCount = CleanupCount + 10; }
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
        Check(ReferenceCatch(source) == 11,
            "the CLR reference runs nested local-throw cleanup inside-out");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "nested local-throw cleanup failed to compile");
        GuestModule module = compiled!.Module;
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        Check(fail.Blocks.Count(block => block.Instructions.Any(item =>
                item.Op == "global_store")) == 2
            && fail.Blocks.Any(block => block.Instructions.Any(item =>
                item.Op == "managed_new"))
            && GuestModuleValidator.Validate(module).Succeeded,
            "nested cleanup must preserve both effects and the original error root");
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, fail.Id, 2);
        GuestModule probe = AddCatchProbe(
            AddProbe(stressed, fail, appendTarget: false,
                "function:nested_local_throw_source_probe",
                "nested_local_throw_source_probe"),
            handler, "function:nested_local_throw_catch_probe",
            "nested_local_throw_catch_probe");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "nested local-throw cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "nested-local-throw-finally.wasm"),
                wasm.Bytes);
        }

        const string branchingSource = """
            class Script
            {
                static int Count;
                static int Fail()
                {
                    try
                    {
                        try { throw new System.Exception(); }
                        finally { Count = Count + 1; }
                    }
                    finally { if (Count == 1) Count = Count + 10; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(branchingSource),
                new string('a', 64), out _, out string? branchingError)
            && branchingError is not null
            && branchingError.Contains("cleanup", StringComparison.Ordinal),
            "branching nested cleanup must fail closed");
    }

    private static void MultipleLocalThrowsShareFinallyAndKeepSources()
    {
        const string source = """
            class Script
            {
                static int Choice;
                static int CleanupCount;
                static int Fail()
                {
                    try
                    {
                        if (Choice == 0) throw new System.Exception();
                        if (Choice == 1) throw new System.Exception();
                        throw new System.Exception();
                    }
                    finally { CleanupCount = CleanupCount + 1; }
                }
                static int CatchFirst()
                {
                    Choice = 0;
                    CleanupCount = 0;
                    try { return Fail(); }
                    catch (System.Exception) { return CleanupCount + 10; }
                }
                static int CatchSecond()
                {
                    Choice = 1;
                    CleanupCount = 0;
                    try { return Fail(); }
                    catch (System.Exception) { return CleanupCount + 20; }
                }
                static int CatchThird()
                {
                    Choice = 2;
                    CleanupCount = 0;
                    try { return Fail(); }
                    catch (System.Exception) { return CleanupCount + 30; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { CatchFirst(); CatchSecond(); CatchThird(); }
            }
            """;
        Check(ReferenceCatch(source, "CatchFirst") == 11
            && ReferenceCatch(source, "CatchSecond") == 21
            && ReferenceCatch(source, "CatchThird") == 31,
            "the CLR reference runs one cleanup after each selected throw");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "shared cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 3 } catalog
            && catalog.Sources.Select(item => item.Token).SequenceEqual(new[] { 1, 2, 3 })
            && catalog.Sources.All(item =>
                source.Substring(item.Start, item.Length)
                    == "throw new System.Exception();")
            && GuestModuleValidator.Validate(module).Succeeded,
            "each local throw needs its own source token and a valid shared outcome");
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, fail.Id, 1);
        GuestModule probe = AddProbe(stressed, fail, appendTarget: false,
            "function:multi_local_cleanup_source_probe",
            "multi_local_cleanup_source_probe");
        foreach ((string method, string suffix) in new[]
        {
            ("CatchFirst", "first"),
            ("CatchSecond", "second"),
            ("CatchThird", "third"),
        })
        {
            GuestFunction handler = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, handler,
                "function:multi_local_cleanup_" + suffix + "_probe",
                "multi_local_cleanup_" + suffix + "_probe");
        }
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "the shared cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "multi-local-throw-finally.wasm"),
                wasm.Bytes);
        }

        const string sideEffectSource = """
            class Script
            {
                static int Choice;
                static int Count;
                static int Fail()
                {
                    try
                    {
                        if (++Choice == 1) throw new System.Exception();
                        throw new System.Exception();
                    }
                    finally { Count = Count + 1; }
                }
                static int CatchFirst()
                {
                    Choice = 0; Count = 0;
                    try { return Fail(); }
                    catch (System.Exception) { return Choice * 10 + Count; }
                }
                static int CatchSecond()
                {
                    Choice = 1; Count = 0;
                    try { return Fail(); }
                    catch (System.Exception) { return Choice * 10 + Count; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { CatchFirst(); CatchSecond(); }
            }
            """;
        Check(ReferenceCatch(sideEffectSource, "CatchFirst") == 11
            && ReferenceCatch(sideEffectSource, "CatchSecond") == 21,
            "the CLR reference evaluates each increment once before cleanup");
        SemanticDocument sideEffectSemantic = Analyze(sideEffectSource);
        Check(CSharpLanguageErrorCompiler.TryLower(sideEffectSemantic,
                new string('a', 64), out CSharpLanguageErrorCompilation? sideEffect,
                out string? sideEffectError) && sideEffect is not null,
            sideEffectError ?? "bounded side-effecting throw decision did not lower");
        GuestModule sideEffectProbe = sideEffect!.Module;
        foreach ((string method, string suffix) in new[]
        {
            ("CatchFirst", "first"),
            ("CatchSecond", "second"),
        })
        {
            GuestFunction handler = sideEffect.Module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            sideEffectProbe = AddCatchProbe(sideEffectProbe, handler,
                "function:side_effect_" + suffix + "_probe",
                "side_effect_" + suffix + "_probe");
        }
        WasmCompilationResult sideEffectWasm = WasmModuleCompiler.Compile(sideEffectProbe);
        Check(sideEffectWasm.Succeeded && sideEffectWasm.Bytes.Length > 8,
            "bounded side-effecting throw decision must compile to WASM");
        if (!string.IsNullOrWhiteSpace(output))
            File.WriteAllBytes(Path.Combine(output, "side-effect-throw-finally.wasm"),
                sideEffectWasm.Bytes);

        const string trappingSource = """
            class Script
            {
                static int Choice;
                static int Divisor;
                static int Count;
                static int Fail()
                {
                    try
                    {
                        if (Choice / Divisor == 0) throw new System.Exception();
                        throw new System.Exception();
                    }
                    finally { Count = Count + 1; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(trappingSource),
                new string('a', 64), out _, out string? trappingError)
            && trappingError is not null,
            "a trapping decision must not masquerade as a pure throw branch");
    }

    private static void NormalReturnsAndLocalThrowsRunTheSameFinally()
    {
        const string source = """
            class Script
            {
                static int Choice;
                static int Count;
                static int Run()
                {
                    try
                    {
                        if (Choice == 0) return Count + 7;
                        if (Choice == 1) throw new System.Exception();
                        if (Choice == 2) return Count + 20;
                        throw new System.Exception();
                    }
                    finally { Count = Count + 1; }
                }
                static int NormalFirst()
                {
                    Choice = 0; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int ErrorFirst()
                {
                    Choice = 1; Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 10; }
                }
                static int NormalSecond()
                {
                    Choice = 2; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int ErrorSecond()
                {
                    Choice = 3; Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 20; }
                }
                static int UncaughtFirst() { Choice = 1; Count = 0; return Run(); }
                static int UncaughtSecond() { Choice = 3; Count = 0; return Run(); }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay()
                {
                    NormalFirst(); ErrorFirst(); NormalSecond(); ErrorSecond();
                }
            }
            """;
        Check(ReferenceCatch(source, "NormalFirst") == 107
            && ReferenceCatch(source, "ErrorFirst") == 11
            && ReferenceCatch(source, "NormalSecond") == 120
            && ReferenceCatch(source, "ErrorSecond") == 21,
            "CLR must evaluate return values before running the shared cleanup");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "mixed cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 2 }
            && GuestModuleValidator.Validate(module).Succeeded,
            "mixed exits need two source tokens and a valid outcome graph");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, 3);
        foreach ((string method, string suffix) in new[]
        {
            ("NormalFirst", "normal_first"),
            ("ErrorFirst", "error_first"),
            ("NormalSecond", "normal_second"),
            ("ErrorSecond", "error_second"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, caller,
                "function:mixed_cleanup_" + suffix + "_probe",
                "mixed_cleanup_" + suffix + "_probe");
        }
        foreach ((string method, string suffix) in new[]
        {
            ("UncaughtFirst", "source_first"),
            ("UncaughtSecond", "source_second"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddProbe(probe, caller, appendTarget: false,
                "function:mixed_cleanup_" + suffix + "_probe",
                "mixed_cleanup_" + suffix + "_probe");
        }
        Check(GuestModuleValidator.Validate(probe).Succeeded,
            "mixed cleanup probes must preserve the Guest outcome contract");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "mixed normal/error cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "mixed-local-throw-finally.wasm"),
                wasm.Bytes);
        }

        const string callInReturn = """
            class Script
            {
                static int Choice;
                static int Count;
                static int Get() => 7;
                static int Run()
                {
                    try
                    {
                        if (Choice == 0) return Get();
                        throw new System.Exception();
                    }
                    finally { Count = Count + 1; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(callInReturn),
                new string('a', 64), out _, out string? callError)
            && callError is not null,
            "a return call that may bypass cleanup must fail closed");

        const string propertyInReturn = """
            class Script
            {
                static int Choice;
                static int Count;
                static int Value { get { return 7; } }
                static int Run()
                {
                    try
                    {
                        if (Choice == 0) return Value;
                        throw new System.Exception();
                    }
                    finally { Count = Count + 1; }
                }
            }
            """;
        Check(!CSharpLanguageErrorCompiler.TryLower(Analyze(propertyInReturn),
                new string('a', 64), out _, out string? propertyError)
            && propertyError is not null
            && propertyError.Contains("synchronous cleanup", StringComparison.Ordinal),
            $"a hidden getter call in a return must not bypass cleanup: {propertyError}");
    }

    private static void MixedExitsRunBranchingFinally()
    {
        const string source = """
            class Script
            {
                static int Choice;
                static int Count;
                static bool FailGet;
                static int Fail() { throw new System.Exception(); }
                static int Get()
                {
                    if (FailGet) return Fail();
                    return 7;
                }
                static int Run()
                {
                    try
                    {
                        if (Choice == 0) return Count + 7;
                        if (Choice == 1) throw new System.Exception();
                        if (Choice == 2) return Get() + 20;
                        throw new System.Exception();
                    }
                    finally
                    {
                        if (Choice < 2) Count = Count + 1;
                        else Count = Count + 2;
                        Count = Count + 10;
                    }
                }
                static int NormalFirst()
                {
                    Choice = 0; Count = 0; FailGet = false;
                    int value = Run();
                    return value + Count * 100;
                }
                static int ErrorFirst()
                {
                    Choice = 1; Count = 0; FailGet = false;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 10; }
                }
                static int NormalSecond()
                {
                    Choice = 2; Count = 0; FailGet = false;
                    int value = Run();
                    return value + Count * 100;
                }
                static int ErrorSecond()
                {
                    Choice = 3; Count = 0; FailGet = false;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 20; }
                }
                static int CalledError()
                {
                    Choice = 2; Count = 0; FailGet = true;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 30; }
                }
                static int UncaughtFirst()
                {
                    Choice = 1; Count = 0; FailGet = false; return Run();
                }
                static int UncaughtSecond()
                {
                    Choice = 3; Count = 0; FailGet = false; return Run();
                }
                static int UncaughtCalled()
                {
                    Choice = 2; Count = 0; FailGet = true; return Run();
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay()
                {
                    NormalFirst(); ErrorFirst(); NormalSecond();
                    ErrorSecond(); CalledError();
                }
            }
            """;
        Check(ReferenceCatch(source, "NormalFirst") == 1107
            && ReferenceCatch(source, "ErrorFirst") == 21
            && ReferenceCatch(source, "NormalSecond") == 1227
            && ReferenceCatch(source, "ErrorSecond") == 32
            && ReferenceCatch(source, "CalledError") == 42,
            "CLR must evaluate returns before either cleanup branch and propagate all errors");
        Check(CSharpLanguageErrorCompiler.TryLower(Analyze(source), new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "mixed branching cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 3 }
            && GuestModuleValidator.Validate(module).Succeeded,
            "mixed branching exits need three source tokens and a valid outcome graph");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, 6);
        foreach ((string method, string suffix) in new[]
        {
            ("NormalFirst", "normal_first"),
            ("ErrorFirst", "error_first"),
            ("NormalSecond", "normal_second"),
            ("ErrorSecond", "error_second"),
            ("CalledError", "called_error"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, caller,
                "function:mixed_branch_" + suffix + "_probe",
                "mixed_branch_" + suffix + "_probe");
        }
        foreach ((string method, string suffix) in new[]
        {
            ("UncaughtFirst", "source_first"),
            ("UncaughtSecond", "source_second"),
            ("UncaughtCalled", "source_called"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddProbe(probe, caller, appendTarget: false,
                "function:mixed_branch_" + suffix + "_probe",
                "mixed_branch_" + suffix + "_probe");
        }
        Check(GuestModuleValidator.Validate(probe).Succeeded,
            "mixed branching probes must preserve the Guest outcome contract");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "mixed branching cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "mixed-branching-finally.wasm"),
                wasm.Bytes);
        }
    }

    private static void CalledReturnAndLocalThrowRunTheSameFinally()
    {
        const string source = """
            class Script
            {
                static int Choice;
                static int Count;
                static int Calls;
                static int Fail() { throw new System.Exception(); }
                static int Get()
                {
                    Calls = Calls + 1;
                    if (Choice == 0) return 7;
                    if (Calls == 1) return 7;
                    return Fail();
                }
                static int Run()
                {
                    try
                    {
                        if (Choice < 2) return Get() + Get();
                        throw new System.Exception();
                    }
                    finally { Count = Count + 1; }
                }
                static int Normal()
                {
                    Choice = 0; Count = 0; Calls = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int CalledError()
                {
                    Choice = 1; Count = 0; Calls = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 10; }
                }
                static int LocalError()
                {
                    Choice = 2; Count = 0; Calls = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 20; }
                }
                static int UncaughtCalled() { Choice = 1; Count = 0; Calls = 0; return Run(); }
                static int UncaughtLocal() { Choice = 2; Count = 0; Calls = 0; return Run(); }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Normal(); CalledError(); LocalError(); }
            }
            """;
        Check(ReferenceCatch(source, "Normal") == 114
            && ReferenceCatch(source, "CalledError") == 11
            && ReferenceCatch(source, "LocalError") == 21,
            "CLR must run cleanup once after a called return succeeds or fails");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "called return cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 2 } catalog
            && catalog.Sources.Select(item => item.Token).SequenceEqual(new[] { 1, 2 })
            && GuestModuleValidator.Validate(module).Succeeded,
            "called and local errors need distinct source tokens and a valid outcome graph");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, 4);
        foreach ((string method, string suffix) in new[]
        {
            ("Normal", "normal"),
            ("CalledError", "called_error"),
            ("LocalError", "local_error"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, caller,
                "function:called_return_" + suffix + "_probe",
                "called_return_" + suffix + "_probe");
        }
        foreach ((string method, string suffix) in new[]
        {
            ("UncaughtCalled", "source_called"),
            ("UncaughtLocal", "source_local"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddProbe(probe, caller, appendTarget: false,
                "function:called_return_" + suffix + "_probe",
                "called_return_" + suffix + "_probe");
        }
        Check(GuestModuleValidator.Validate(probe).Succeeded,
            "called-return probes must preserve the Guest outcome contract");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "called-return cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "called-return-finally.wasm"),
                wasm.Bytes);
        }
    }

    private static void CalledReturnRunsBranchingFinally()
    {
        const string source = """
            class Script
            {
                static bool UseFirst;
                static int Mode;
                static int Count;
                static int Fail() { throw new System.Exception(); }
                static int Get()
                {
                    if (Mode == 0) return 7;
                    return Fail();
                }
                static int Run()
                {
                    try { return Get(); }
                    finally
                    {
                        if (UseFirst) { Count = Count + 1; }
                        else { Count = Count + 2; }
                        Count = Count + 10;
                    }
                }
                static int RunNoElse()
                {
                    try { return Get(); }
                    finally { if (UseFirst) Count = Count + 1; }
                }
                static int RunNested()
                {
                    try
                    {
                        try { return Get(); }
                        finally
                        {
                            if (UseFirst) Count = Count + 1;
                            else Count = Count + 2;
                        }
                    }
                    finally { Count = Count + 100; }
                }
                static int Normal()
                {
                    Mode = 0; UseFirst = true; Count = 0;
                    return Run() + Count * 100;
                }
                static int FirstError()
                {
                    Mode = 1; UseFirst = true; Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                static int SecondError()
                {
                    Mode = 1; UseFirst = false; Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                static int NoElseTaken()
                {
                    Mode = 1; UseFirst = true; Count = 0;
                    try { return RunNoElse(); }
                    catch (System.Exception) { return Count; }
                }
                static int NoElseSkipped()
                {
                    Mode = 1; UseFirst = false; Count = 0;
                    try { return RunNoElse(); }
                    catch (System.Exception) { return Count; }
                }
                static int NestedNormal()
                {
                    Mode = 0; UseFirst = true; Count = 0;
                    return RunNested() + Count * 100;
                }
                static int NestedFirstError()
                {
                    Mode = 1; UseFirst = true; Count = 0;
                    try { return RunNested(); }
                    catch (System.Exception) { return Count; }
                }
                static int NestedSecondError()
                {
                    Mode = 1; UseFirst = false; Count = 0;
                    try { return RunNested(); }
                    catch (System.Exception) { return Count; }
                }
                static int Uncaught()
                {
                    Mode = 1; UseFirst = true; Count = 0;
                    return Run();
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay()
                {
                    Normal(); FirstError(); SecondError(); NoElseTaken(); NoElseSkipped();
                    NestedNormal(); NestedFirstError(); NestedSecondError();
                }
            }
            """;
        Check(ReferenceCatch(source, "Normal") == 1107
            && ReferenceCatch(source, "FirstError") == 11
            && ReferenceCatch(source, "SecondError") == 12
            && ReferenceCatch(source, "NoElseTaken") == 1
            && ReferenceCatch(source, "NoElseSkipped") == 0
            && ReferenceCatch(source, "NestedNormal") == 10107
            && ReferenceCatch(source, "NestedFirstError") == 101
            && ReferenceCatch(source, "NestedSecondError") == 102,
            "CLR must execute either finally branch after normal and error returns");
        Check(CSharpLanguageErrorCompiler.TryLower(Analyze(source), new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "called-error branching finally failed to compile");
        GuestModule module = compiled!.Module;
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        Check(run.Blocks.Count(block => block.Terminator.Kind == "branch_if") == 3
            && run.Blocks.Count(block => block.Instructions.Any(instruction =>
                instruction.Op == "global_store")) == 6
            && GuestModuleValidator.Validate(module).Succeeded,
            "normal and error paths need independent copies of the branching cleanup");
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, 6);
        GuestFunction noElse = module.Functions.Single(function =>
            function.Id.Contains(".RunNoElse(", StringComparison.Ordinal));
        Check(noElse.Blocks.Count(block => block.Terminator.Kind == "branch_if") == 3
            && noElse.Blocks.Count(block => block.Instructions.Any(instruction =>
                instruction.Op == "global_store")) == 2,
            "single-arm cleanup needs both taken and skipped error edges");
        probe = AddLocalCleanupCollectProbe(probe, noElse.Id, 2);
        GuestFunction nested = module.Functions.Single(function =>
            function.Id.Contains(".RunNested(", StringComparison.Ordinal));
        probe = AddLocalCleanupCollectProbe(probe, nested.Id, 6);
        foreach ((string method, string suffix) in new[]
        {
            ("Normal", "normal"),
            ("FirstError", "first"),
            ("SecondError", "second"),
            ("NoElseTaken", "no_else_taken"),
            ("NoElseSkipped", "no_else_skipped"),
            ("NestedNormal", "nested_normal"),
            ("NestedFirstError", "nested_first"),
            ("NestedSecondError", "nested_second"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, caller,
                "function:called_branch_" + suffix + "_probe",
                "called_branch_" + suffix + "_probe");
        }
        GuestFunction uncaught = module.Functions.Single(function =>
            function.Id.Contains(".Uncaught(", StringComparison.Ordinal));
        probe = AddProbe(probe, uncaught, appendTarget: false,
            "function:called_branch_source_probe", "called_branch_source_probe");
        Check(GuestModuleValidator.Validate(probe).Succeeded,
            "called branching cleanup probes must preserve the outcome contract");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "called branching cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "called-branching-finally.wasm"), wasm.Bytes);
        }
    }

    private static void CatchReturnsRunOuterFinally()
    {
        const string source = """
            class Script
            {
                static int Choice;
                static int Count;
                static int FailTry() { throw new System.Exception(); }
                static int FailCatch() { throw new System.Exception(); }
                static int Maybe()
                {
                    if (Choice == 0) return 5;
                    return FailTry();
                }
                static int Recover()
                {
                    if (Choice == 1) return 7;
                    return FailCatch();
                }
                static int Run()
                {
                    try { return Maybe(); }
                    catch (System.Exception) { return Recover(); }
                    finally { Count = Count + 1; }
                }
                static int Normal()
                {
                    Choice = 0; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int Handled()
                {
                    Choice = 1; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int Escaped()
                {
                    Choice = 2; Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 20; }
                }
                static int Uncaught() { Choice = 2; Count = 0; return Run(); }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Normal(); Handled(); Escaped(); }
            }
            """;
        Check(ReferenceCatch(source, "Normal") == 105
            && ReferenceCatch(source, "Handled") == 107
            && ReferenceCatch(source, "Escaped") == 21,
            "CLR must run the outer finally after try, catch, and escaped catch errors");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "catch cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 2 } catalog
            && catalog.Sources.Select(item => item.Token).SequenceEqual(new[] { 1, 2 })
            && GuestModuleValidator.Validate(module).Succeeded,
            "catch cleanup must retain both source errors and a valid outcome graph");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, 5);
        foreach ((string method, string suffix) in new[]
        {
            ("Normal", "normal"),
            ("Handled", "handled"),
            ("Escaped", "escaped"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, caller,
                "function:catch_finally_" + suffix + "_probe",
                "catch_finally_" + suffix + "_probe");
        }
        GuestFunction uncaught = module.Functions.Single(function =>
            function.Id.Contains(".Uncaught(", StringComparison.Ordinal));
        probe = AddProbe(probe, uncaught, appendTarget: false,
            "function:catch_finally_source_probe", "catch_finally_source_probe");
        Check(GuestModuleValidator.Validate(probe).Succeeded,
            "catch-finally probes must preserve the Guest outcome contract");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "catch-finally must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-finally.wasm"), wasm.Bytes);
        }
    }

    private static void CatchReturnsRunBranchingFinally()
    {
        const string source = """
            class Script
            {
                static int Choice;
                static bool UseFirst;
                static int Count;
                static int FailTry() { throw new System.Exception(); }
                static int FailCatch() { throw new System.Exception(); }
                static int Maybe()
                {
                    if (Choice == 0) return 5;
                    return FailTry();
                }
                static int Recover()
                {
                    if (Choice == 1) return 7;
                    return FailCatch();
                }
                static int Run()
                {
                    try { return Maybe(); }
                    catch (System.Exception) { return Recover(); }
                    finally
                    {
                        if (UseFirst) Count = Count + 1;
                        else Count = Count + 2;
                        Count = Count + 10;
                    }
                }
                static int NormalFirst()
                {
                    Choice = 0; UseFirst = true; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int NormalSecond()
                {
                    Choice = 0; UseFirst = false; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int HandledFirst()
                {
                    Choice = 1; UseFirst = true; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int HandledSecond()
                {
                    Choice = 1; UseFirst = false; Count = 0;
                    int value = Run();
                    return value + Count * 100;
                }
                static int EscapedFirst()
                {
                    Choice = 2; UseFirst = true; Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 20; }
                }
                static int EscapedSecond()
                {
                    Choice = 2; UseFirst = false; Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count + 20; }
                }
                static int UncaughtFirst()
                {
                    Choice = 2; UseFirst = true; Count = 0; return Run();
                }
                static int UncaughtSecond()
                {
                    Choice = 2; UseFirst = false; Count = 0; return Run();
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay()
                {
                    NormalFirst(); NormalSecond(); HandledFirst();
                    HandledSecond(); EscapedFirst(); EscapedSecond();
                }
            }
            """;
        Check(ReferenceCatch(source, "NormalFirst") == 1105
            && ReferenceCatch(source, "NormalSecond") == 1205
            && ReferenceCatch(source, "HandledFirst") == 1107
            && ReferenceCatch(source, "HandledSecond") == 1207
            && ReferenceCatch(source, "EscapedFirst") == 31
            && ReferenceCatch(source, "EscapedSecond") == 32,
            "CLR must run the selected outer finally branch after try, catch, and escaping errors");
        Check(CSharpLanguageErrorCompiler.TryLower(Analyze(source), new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "catch branching cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 2 }
            && GuestModuleValidator.Validate(module).Succeeded,
            "catch branching cleanup needs two source errors and a valid outcome graph");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        int cleanupBlocks = run.Blocks.Count(block => block.Instructions.Any(
            instruction => instruction.Op == "global_store"));
        Check(cleanupBlocks >= 6 && cleanupBlocks % 3 == 0,
            "catch branching cleanup must retain both arms and their cloned error paths");
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, cleanupBlocks);
        foreach ((string method, string suffix) in new[]
        {
            ("NormalFirst", "normal_first"),
            ("NormalSecond", "normal_second"),
            ("HandledFirst", "handled_first"),
            ("HandledSecond", "handled_second"),
            ("EscapedFirst", "escaped_first"),
            ("EscapedSecond", "escaped_second"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, caller,
                "function:catch_branch_" + suffix + "_probe",
                "catch_branch_" + suffix + "_probe");
        }
        foreach ((string method, string suffix) in new[]
        {
            ("UncaughtFirst", "source_first"),
            ("UncaughtSecond", "source_second"),
        })
        {
            GuestFunction caller = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddProbe(probe, caller, appendTarget: false,
                "function:catch_branch_" + suffix + "_probe",
                "catch_branch_" + suffix + "_probe");
        }
        Check(GuestModuleValidator.Validate(probe).Succeeded,
            "catch branching probes must preserve the Guest outcome contract");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "catch branching cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-branching-finally.wasm"),
                wasm.Bytes);
        }
    }

    private static void NestedCleanupThrowReplacesOriginalError()
    {
        const string source = """
            class Script
            {
                static int Count;
                static int Fail()
                {
                    try
                    {
                        try { throw new System.Exception(); }
                        finally { Count = Count + 1; throw new System.Exception(); }
                    }
                    finally { Count = Count + 10; }
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
        Check(ReferenceCatch(source) == 11,
            "the CLR reference must replace the inner error and run outer cleanup");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "nested cleanup replacement failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 2 } catalog
            && catalog.Sources[0].Start < catalog.Sources[1].Start
            && source.Substring(catalog.Sources[0].Start, catalog.Sources[0].Length)
                == "throw new System.Exception();"
            && source.Substring(catalog.Sources[1].Start, catalog.Sources[1].Length)
                == "throw new System.Exception();",
            "nested replacement must retain both source-backed throw identities");
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
            reorderedError ?? "nested replacement must not depend on throw-site order");
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        Check(fail.Blocks.Sum(block => block.Instructions.Count(item =>
                item.Op == "managed_new")) == 2
            && fail.Blocks.Count(block => block.Instructions.Any(item =>
                item.Op == "global_store")) == 2,
            "nested replacement must allocate both errors and run both cleanup blocks");
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, fail.Id, 2);
        GuestModule probe = AddCatchProbe(
            AddProbe(stressed, fail, appendTarget: false,
                "function:nested_replacement_source_probe",
                "nested_replacement_source_probe"),
            handler, "function:nested_replacement_catch_probe",
            "nested_replacement_catch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "nested cleanup replacement must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output,
                "nested-cleanup-replaces-error.wasm"), wasm.Bytes);
        }
    }

    private static void OuterNestedCleanupThrowReplacesOriginalError()
    {
        const string source = """
            class Script
            {
                static int Count;
                static int Fail()
                {
                    try
                    {
                        try
                        {
                            try { throw new System.Exception(); }
                            finally { Count = Count + 1; }
                        }
                        finally { Count = Count + 10; throw new System.Exception(); }
                    }
                    finally { Count = Count + 100; }
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
        Check(ReferenceCatch(source) == 111,
            "the CLR reference must run inner cleanup, replace its error in the middle, then run outer cleanup");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "outer nested cleanup replacement failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 2 },
            "outer nested replacement must keep both source-backed throw identities");
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        Check(fail.Blocks.Sum(block => block.Instructions.Count(item =>
                item.Op == "managed_new")) == 2
            && fail.Blocks.Count(block => block.Instructions.Any(item =>
                item.Op == "global_store")) == 3,
            "outer nested replacement must allocate both errors and run three cleanup blocks");
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, fail.Id, 3);
        GuestModule probe = AddCatchProbe(
            AddProbe(stressed, fail, appendTarget: false,
                "function:outer_nested_replacement_source_probe",
                "outer_nested_replacement_source_probe"),
            handler, "function:outer_nested_replacement_catch_probe",
            "outer_nested_replacement_catch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "outer nested cleanup replacement must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output,
                "outer-nested-cleanup-replaces-error.wasm"), wasm.Bytes);
        }
    }

    private static void OutermostCleanupThrowReplacesOriginalError()
    {
        const string source = """
            class Script
            {
                static int Count;
                static int Fail()
                {
                    try
                    {
                        try { throw new System.Exception(); }
                        finally { Count = Count + 1; }
                    }
                    finally { Count = Count + 10; throw new System.Exception(); }
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
        Check(ReferenceCatch(source) == 11,
            "the CLR reference must replace the error in the outermost cleanup");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "outermost cleanup replacement failed to compile");
        GuestModule module = compiled!.Module;
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, fail.Id, 2);
        GuestModule probe = AddCatchProbe(
            AddProbe(stressed, fail, appendTarget: false,
                "function:outermost_replacement_source_probe",
                "outermost_replacement_source_probe"),
            handler, "function:outermost_replacement_catch_probe",
            "outermost_replacement_catch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "outermost cleanup replacement must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output,
                "outermost-cleanup-replaces-error.wasm"), wasm.Bytes);
        }
    }

    private static void ConsecutiveNestedCleanupThrowsKeepLastError()
    {
        const string source = """
            class Script
            {
                static int Count;
                static int Fail()
                {
                    try
                    {
                        try { throw new System.Exception(); }
                        finally { Count = Count + 1; throw new System.Exception(); }
                    }
                    finally { Count = Count + 10; throw new System.Exception(); }
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
        Check(ReferenceCatch(source) == 11,
            "the CLR reference must execute both throwing cleanup blocks");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null,
            error ?? "consecutive nested cleanup throws failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Sources.Count: 3 },
            "each replacing throw needs its own source token");
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        Check(fail.Blocks.Sum(block => block.Instructions.Count(item =>
                item.Op == "managed_new")) == 3,
            "consecutive cleanup throws must allocate three distinct errors");
        GuestFunction handler = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, fail.Id, 2);
        GuestModule probe = AddCatchProbe(
            AddProbe(stressed, fail, appendTarget: false,
                "function:consecutive_replacement_source_probe",
                "consecutive_replacement_source_probe"),
            handler, "function:consecutive_replacement_catch_probe",
            "consecutive_replacement_catch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "consecutive nested cleanup throws must compile to WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output,
                "consecutive-nested-cleanup-replaces-error.wasm"), wasm.Bytes);
        }
    }

    private static void BranchingFinallyRunsBeforeErrorPropagation()
    {
        const string source = """
            class Script
            {
                static bool UseFirst;
                static int Count;
                static int Fail()
                {
                    try { throw new System.Exception(); }
                    finally
                    {
                        if (UseFirst) { Count = Count + 1; }
                        else { Count = Count + 2; }
                        Count = Count + 10;
                    }
                }
                static int First()
                {
                    Count = 0;
                    UseFirst = true;
                    try { return Fail(); }
                    catch (System.Exception) { return Count; }
                }
                static int Second()
                {
                    Count = 0;
                    UseFirst = false;
                    try { return Fail(); }
                    catch (System.Exception) { return Count; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { First(); Second(); }
            }
            """;
        Check(ReferenceCatch(source, "First") == 11
            && ReferenceCatch(source, "Second") == 12,
            "the CLR reference must run both branches before propagating the error");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "branching cleanup failed to compile");
        GuestModule module = compiled!.Module;
        GuestFunction fail = module.Functions.Single(function =>
            function.Id.Contains(".Fail(", StringComparison.Ordinal));
        Check(fail.Blocks.Count(block => block.Terminator.Kind == "branch_if") == 1
            && fail.Blocks.Count(block => block.Instructions.Any(item =>
                item.Op == "global_store")) == 3
            && fail.Blocks.Sum(block => block.Instructions.Count(item =>
                item.Op == "managed_new")) == 1,
            "the branching finally must preserve both paths and one error root");
        GuestFunction first = module.Functions.Single(function =>
            function.Id.Contains(".First(", StringComparison.Ordinal));
        GuestFunction second = module.Functions.Single(function =>
            function.Id.Contains(".Second(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, fail.Id, 3);
        GuestModule probe = AddCatchProbe(AddCatchProbe(
                AddProbe(stressed, fail, appendTarget: false,
                    "function:branch_cleanup_source_probe",
                    "branch_cleanup_source_probe"),
                first, "function:branch_cleanup_first_probe", "branch_cleanup_first_probe"),
            second, "function:branch_cleanup_second_probe", "branch_cleanup_second_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "branching cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "branching-cleanup.wasm"), wasm.Bytes);
        }
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

    private static void CatchRethrowRunsOuterFinally()
    {
        const string source = """
            class Script
            {
                static bool FailNow;
                static int Count;
                static int Fail() { throw new System.Exception(); }
                static int Choose()
                {
                    if (FailNow) return Fail();
                    return 7;
                }
                static int Run()
                {
                    try { return Choose(); }
                    catch (System.Exception) { throw; }
                    finally { Count = Count + 10; }
                }
                static int Normal()
                {
                    Count = 0;
                    FailNow = false;
                    return Run() + Count * 100;
                }
                static int Error()
                {
                    Count = 0;
                    FailNow = true;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Normal(); Error(); }
            }
            """;
        Check(ReferenceCatch(source, "Normal") == 1007
            && ReferenceCatch(source, "Error") == 10,
            "the CLR reference must run outer cleanup on normal return and catch rethrow");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "catch rethrow cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 },
            "the rethrow must not allocate a second source token");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestFunction normal = module.Functions.Single(function =>
            function.Id.Contains(".Normal(", StringComparison.Ordinal));
        GuestFunction handled = module.Functions.Single(function =>
            function.Id.Contains(".Error(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, run.Id, 3);
        GuestModule probe = AddCatchProbe(AddCatchProbe(
                AddProbe(stressed, run, appendTarget: false,
                    "function:rethrow_finally_source_probe", "rethrow_finally_source_probe"),
                normal, "function:rethrow_finally_normal_probe",
                "rethrow_finally_normal_probe"),
            handled, "function:rethrow_finally_error_probe",
            "rethrow_finally_error_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "catch rethrow cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-rethrow-finally.wasm"),
                wasm.Bytes);
        }
    }

    private static void CatchRethrowRunsBranchingFinally()
    {
        const string source = """
            class Script
            {
                static bool FailNow;
                static bool UseFirst;
                static int Count;
                static int Fail() { throw new System.Exception(); }
                static int Choose()
                {
                    if (FailNow) return Fail();
                    return 7;
                }
                static int Run()
                {
                    try { return Choose(); }
                    catch (System.Exception) { throw; }
                    finally
                    {
                        if (UseFirst) { Count = Count + 1; }
                        else { Count = Count + 2; }
                        Count = Count + 10;
                    }
                }
                static int NormalFirst()
                {
                    Count = 0; FailNow = false; UseFirst = true;
                    return Run() + Count * 100;
                }
                static int NormalSecond()
                {
                    Count = 0; FailNow = false; UseFirst = false;
                    return Run() + Count * 100;
                }
                static int ErrorFirst()
                {
                    Count = 0; FailNow = true; UseFirst = true;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                static int ErrorSecond()
                {
                    Count = 0; FailNow = true; UseFirst = false;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay()
                {
                    NormalFirst(); NormalSecond(); ErrorFirst(); ErrorSecond();
                }
            }
            """;
        Check(ReferenceCatch(source, "NormalFirst") == 1107
            && ReferenceCatch(source, "NormalSecond") == 1207
            && ReferenceCatch(source, "ErrorFirst") == 11
            && ReferenceCatch(source, "ErrorSecond") == 12,
            "the CLR reference must select one cleanup branch on each return or rethrow");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "branching catch rethrow cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 },
            "branching cleanup must preserve the original throw source");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, 6);
        probe = AddProbe(probe, run, appendTarget: false,
            "function:rethrow_branch_source_probe", "rethrow_branch_source_probe");
        foreach ((string method, string export) in new[]
        {
            ("NormalFirst", "rethrow_branch_normal_first_probe"),
            ("NormalSecond", "rethrow_branch_normal_second_probe"),
            ("ErrorFirst", "rethrow_branch_error_first_probe"),
            ("ErrorSecond", "rethrow_branch_error_second_probe"),
        })
        {
            GuestFunction target = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, target, "function:" + export, export);
        }
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "branching catch rethrow cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "catch-rethrow-branching-finally.wasm"),
                wasm.Bytes);
        }
    }

    private static void CatchLocalRethrowRunsOuterFinally()
    {
        const string source = """
            class Script
            {
                static int Count;
                static int Run()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception) { throw; }
                    finally { Count = Count + 10; }
                }
                static int Catch()
                {
                    Count = 0;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { Catch(); }
            }
            """;
        Check(ReferenceCatch(source) == 10,
            "the CLR reference must run cleanup after the same method catches and rethrows");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "local catch rethrow cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 },
            "local rethrow must reuse its original source token");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestFunction handled = module.Functions.Single(function =>
            function.Id.Contains(".Catch(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, run.Id, 1);
        GuestModule probe = AddCatchProbe(
            AddProbe(stressed, run, appendTarget: false,
                "function:local_rethrow_finally_source_probe",
                "local_rethrow_finally_source_probe"),
            handled, "function:local_rethrow_finally_catch_probe",
            "local_rethrow_finally_catch_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "local catch rethrow cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "local-catch-rethrow-finally.wasm"),
                wasm.Bytes);
        }
    }

    private static void CatchLocalRethrowRunsBranchingFinally()
    {
        const string source = """
            class Script
            {
                static bool UseFirst;
                static int Count;
                static int Run()
                {
                    try { throw new System.Exception(); }
                    catch (System.Exception) { throw; }
                    finally
                    {
                        if (UseFirst) { Count = Count + 1; }
                        else { Count = Count + 2; }
                        Count = Count + 10;
                    }
                }
                static int First()
                {
                    Count = 0; UseFirst = true;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                static int Second()
                {
                    Count = 0; UseFirst = false;
                    try { return Run(); }
                    catch (System.Exception) { return Count; }
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay() { First(); Second(); }
            }
            """;
        Check(ReferenceCatch(source, "First") == 11
            && ReferenceCatch(source, "Second") == 12,
            "the CLR reference must select one cleanup branch after local rethrow");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null, error ?? "branching local rethrow cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 },
            "branching local rethrow must reuse its original source token");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestFunction first = module.Functions.Single(function =>
            function.Id.Contains(".First(", StringComparison.Ordinal));
        GuestFunction second = module.Functions.Single(function =>
            function.Id.Contains(".Second(", StringComparison.Ordinal));
        GuestModule stressed = AddLocalCleanupCollectProbe(module, run.Id, 3);
        GuestModule probe = AddCatchProbe(AddCatchProbe(
                AddProbe(stressed, run, appendTarget: false,
                    "function:local_rethrow_branch_source_probe",
                    "local_rethrow_branch_source_probe"),
                first, "function:local_rethrow_branch_first_probe",
                "local_rethrow_branch_first_probe"),
            second, "function:local_rethrow_branch_second_probe",
            "local_rethrow_branch_second_probe");
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "branching local rethrow cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output,
                "local-catch-rethrow-branching-finally.wasm"), wasm.Bytes);
        }
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

    private static void NestedCatchRethrowRunsBranchingOuterFinally()
    {
        const string source = """
            class Script
            {
                static bool FailNow;
                static bool UseFirst;
                static int Count;
                static int Fail() { throw new System.Exception(); }
                static int Choose() { if (FailNow) return Fail(); return 7; }
                static int Run()
                {
                    try
                    {
                        try { return Choose(); }
                        catch (System.Exception) { throw; }
                    }
                    catch (System.Exception) { return 5; }
                    finally
                    {
                        if (UseFirst) { Count = Count + 1; }
                        else { Count = Count + 2; }
                        Count = Count + 10;
                    }
                }
                static int NormalFirst()
                {
                    FailNow = false; UseFirst = true; Count = 0;
                    return Run() + Count * 100;
                }
                static int NormalSecond()
                {
                    FailNow = false; UseFirst = false; Count = 0;
                    return Run() + Count * 100;
                }
                static int HandledFirst()
                {
                    FailNow = true; UseFirst = true; Count = 0;
                    return Run() + Count * 100;
                }
                static int HandledSecond()
                {
                    FailNow = true; UseFirst = false; Count = 0;
                    return Run() + Count * 100;
                }
                [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                static void BeginPlay()
                {
                    NormalFirst(); NormalSecond(); HandledFirst(); HandledSecond();
                }
            }
            """;
        Check(ReferenceCatch(source, "NormalFirst") == 1107
            && ReferenceCatch(source, "NormalSecond") == 1207
            && ReferenceCatch(source, "HandledFirst") == 1105
            && ReferenceCatch(source, "HandledSecond") == 1205,
            "the CLR reference must run outer cleanup after normal and nested handled returns");
        SemanticDocument semantic = Analyze(source);
        Check(CSharpLanguageErrorCompiler.TryLower(semantic, new string('a', 64),
                out CSharpLanguageErrorCompilation? compiled, out string? error)
            && compiled is not null,
            error ?? "nested catch branching cleanup failed to compile");
        GuestModule module = compiled!.Module;
        Check(module.LanguageErrorCatalog is { Types.Count: 1, Sources.Count: 1 },
            "nested catch rethrow must retain only the original source token");
        GuestFunction run = module.Functions.Single(function =>
            function.Id.Contains(".Run(", StringComparison.Ordinal));
        GuestModule probe = AddLocalCleanupCollectProbe(module, run.Id, 6);
        foreach ((string method, string exportName) in new[]
        {
            ("NormalFirst", "nested_catch_cleanup_normal_first_probe"),
            ("NormalSecond", "nested_catch_cleanup_normal_second_probe"),
            ("HandledFirst", "nested_catch_cleanup_handled_first_probe"),
            ("HandledSecond", "nested_catch_cleanup_handled_second_probe"),
        })
        {
            GuestFunction handler = module.Functions.Single(function =>
                function.Id.Contains("." + method + "(", StringComparison.Ordinal));
            probe = AddCatchProbe(probe, handler, "function:" + exportName, exportName);
        }
        GuestValidationResult validation = GuestModuleValidator.Validate(probe);
        Check(validation.Succeeded,
            string.Join(" | ", validation.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(probe);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "nested catch branching cleanup must compile to executable WASM");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_THROW_PRODUCER_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "nested-catch-branching-finally.wasm"),
                wasm.Bytes);
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

    private static GuestModule AddLocalCleanupCollectProbe(
        GuestModule module, string functionId, int expectedBlocks)
    {
        GuestFunction function = module.Functions.Single(item => item.Id == functionId);
        GuestBasicBlock[] cleanupBlocks = function.Blocks.Where(block =>
            block.Instructions.Any(instruction => instruction.Op == "global_store")).ToArray();
        Check(cleanupBlocks.Length == expectedBlocks,
            $"local throw needs {expectedBlocks} cleanup blocks, found {cleanupBlocks.Length}");
        IReadOnlySet<string> cleanupIds = cleanupBlocks.Select(block => block.Id)
            .ToHashSet(StringComparer.Ordinal);
        GuestFunction stressed = function with
        {
            Blocks = function.Blocks.Select(block => cleanupIds.Contains(block.Id)
                ? block with
                {
                    Instructions = new[]
                    {
                        new GuestInstruction("managed_collect", null,
                            Array.Empty<string>(), null, null, null),
                    }.Concat(block.Instructions).ToArray(),
                } : block).ToArray(),
        };
        GuestModule result = module with
        {
            Functions = module.Functions.Select(item => item.Id == functionId
                ? stressed : item).ToArray(),
        };
        Check(GuestModuleValidator.Validate(result).Succeeded,
            "the local cleanup GC probe must remain a valid Guest module");
        return result;
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

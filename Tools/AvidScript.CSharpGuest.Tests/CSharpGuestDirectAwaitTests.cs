using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestDirectAwaitTests
{
    public static int Run()
    {
        const string source = """
            using AvidScript;
            using System;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int CleanupCount;
                public static int Result;
                public static async Task<int> RunAsync()
                {
                    try { await AvidContinuations.NextTickAsync(); return 16; }
                    catch (InvalidOperationException) { return 17; }
                    finally { CleanupCount++; }
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay() { Result = await RunAsync(); }
            }
            """;
        const string sourceId = "Scripts/DirectAwaitCleanup.cs";
        const string additionalFacade = """

            internal static class CancelResumeHost
            {
                [System.Runtime.InteropServices.DllImport("avidscript",
                    EntryPoint = "avid_continuation_delay_cancel_resume_v1")]
                internal static extern long Delay(float seconds, int callbackId);
            }
            """;
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId,
            frontend.Source.Sha256, new[]
            {
                new SemanticReferenceSource(
                    CSharpGuestContinuationTests.ReferenceFacade + additionalFacade,
                    "generated://AvidScript.Continuations.generated.cs", true),
            }, new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
            enableDirectAwaitCleanup: true);
        Check(semantic.Succeeded
            && semantic.SchemaVersion == SemanticContract.DirectAwaitCleanupSchemaVersion,
            "protected direct await must produce Semantic 43: "
                + string.Join(" | ", semantic.Diagnostics.Select(item =>
                    item.Code + ":" + item.Message)));
        string semanticHash = new('a', 64);
        Check(!CSharpGuestLowerer.Lower(semantic, semanticHash).Succeeded,
            "the default Guest profile must reject Semantic 43");
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(
            semantic, semanticHash, enableAsyncLanguageErrors: true);
        Check(lowered.Succeeded && lowered.Module is not null,
            "Semantic 43 must lower to IR 23: "
                + string.Join(" | ", lowered.Diagnostics.Select(item =>
                    item.Code + ":" + item.Message)));
        GuestModule module = lowered.Module!;
        Check(module.SchemaVersion == GuestTaskLanguageErrorValidator.DirectCleanupSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.DirectCleanupIrVersion
            && module.DirectAwaitRoutes is { Count: 1 }
            && module.AsyncExceptionRoutes is { Count: 0 }
            && GuestModuleValidator.Validate(module).Succeeded,
            "IR 23 must bind a cancellation-only route without a Task fault lease");
        GuestDirectAwaitRoute route = module.DirectAwaitRoutes![0];
        Check(module.Imports.Any(import => import.Id == route.ScheduleImportId
            && import.Module == "avidscript"
            && import.Name == "avid_continuation_delay_cancel_resume_v1")
            && module.Imports.All(import => import.Name != "continuation_delay")
            && module.Functions.Single(function => function.Id.EndsWith(
                ":" + route.CallbackId, StringComparison.Ordinal)
                && function.Id.Contains("async_resume", StringComparison.Ordinal))
                .Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int64", "type:int32" }),
            "the Timer producer and resume must use the versioned status ABI");
        Check(GuestModuleValidator.Validate(module with
        {
            DirectAwaitRoutes = null,
        }).Diagnostics.Any(item => item.Code == "ASIR1031"),
            "IR 23 must reject a missing direct route");
        Check(GuestModuleValidator.Validate(module with
        {
            DirectAwaitRoutes = new[] { route with
                { CancellationTargetBlockId = route.NormalTargetBlockId } },
        }).Diagnostics.Any(item => item.Code == "ASIR1031"),
            "IR 23 must reject a cancellation edge into normal code");
        GuestFunction resume = module.Functions.Single(function => function.Id
            == "function:synthetic:async_resume:" + route.CallbackId);
        string cancellationPathId = route.NormalTargetBlockId + ":entry:cancel_path";
        GuestFunction forgedResume = resume with
        {
            Blocks = resume.Blocks.Select(block => block.Id == cancellationPathId
                ? block with { Terminator = block.Terminator with
                    { TargetBlockId = route.NormalTargetBlockId } }
                : block).ToArray(),
        };
        Check(GuestModuleValidator.Validate(module with
        {
            Functions = module.Functions.Select(function => function.Id == resume.Id
                ? forgedResume : function).ToArray(),
        }).Diagnostics.Any(item => item.Code == "ASIR1031"),
            "IR 23 must reject a cancelled resume redirected into normal code");
        GuestFunction skippedStatus = resume with
        {
            Blocks = resume.Blocks.Select(block => block.Id == resume.EntryBlockId
                ? block with { Terminator = block.Terminator with
                    { Kind = "branch", ConditionValueId = null,
                        TargetBlockId = route.NormalTargetBlockId,
                        FalseTargetBlockId = null } }
                : block).ToArray(),
        };
        Check(GuestModuleValidator.Validate(module with
        {
            Functions = module.Functions.Select(function => function.Id == resume.Id
                ? skippedStatus : function).ToArray(),
        }).Diagnostics.Any(item => item.Code == "ASIR1031"),
            "IR 23 must reject an entry that bypasses status validation");
        Check(GuestModuleValidator.Validate(module with
        {
            SchemaVersion = GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion,
            IrVersion = GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion,
        }).Diagnostics.Any(item => item.Code == "ASIR1031"),
            "IR 22 must reject a direct status route");
        byte[] serialized = GuestIrSerializer.Serialize(module);
        Check(serialized.SequenceEqual(GuestIrSerializer.Serialize(
            GuestIrSerializer.Deserialize(serialized))),
            "IR 23 must round-trip canonically");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Check(wasm.Succeeded && wasm.Bytes.Length > 8,
            "IR 23 must compile to WASM: " + string.Join(" | ",
                wasm.Diagnostics.Select(item => item.Message)));

        const string mixedSource = """
            using AvidScript;
            using System;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Cleanups;
                private static async Task<int> LoadAsync(bool fail)
                {
                    await AvidContinuations.NextTickAsync();
                    if (fail) throw new InvalidOperationException();
                    return 16;
                }
                public static async Task<int> RunAsync()
                {
                    try
                    {
                        await AvidContinuations.NextTickAsync();
                        int value = await LoadAsync(true);
                        return value;
                    }
                    catch (InvalidOperationException) { return 17; }
                    finally { Cleanups++; }
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay() { await RunAsync(); }
            }
            """;
        const string mixedSourceId = "Scripts/DirectAndTaskAwaitCatch.cs";
        FrontendDocument mixedFrontend = FrontendAnalyzer.Analyze(
            mixedSource, mixedSourceId);
        SemanticDocument mixedSemantic = SemanticAnalyzer.Analyze(
            mixedSource, mixedSourceId, mixedFrontend.Source.Sha256,
            new[] { new SemanticReferenceSource(
                CSharpGuestContinuationTests.ReferenceFacade + additionalFacade,
                "generated://AvidScript.Continuations.generated.cs", true) },
            new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
            enableDirectAwaitCleanup: true);
        Check(mixedSemantic.SchemaVersion == SemanticContract.DirectAwaitCleanupSchemaVersion
            && mixedSemantic.AsyncMethods.Any(method => method.ErrorPlan?.Throws.Count > 0)
            && mixedSemantic.Diagnostics.Where(item => item.Severity == "error")
                .All(item => item.Code == "ASCS5422"),
            "mixed direct and Task awaits must preserve a bounded throw plan: " + string.Join(" | ",
                mixedSemantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        CSharpGuestLoweringResult mixedLowered = CSharpGuestLowerer.Lower(
            mixedSemantic, new string('b', 64), enableAsyncLanguageErrors: true);
        Check(mixedLowered.Succeeded && mixedLowered.Module is { } mixedModule
            && mixedModule.DirectAwaitRoutes is { Count: 1 }
            && mixedModule.AsyncExceptionRoutes is { Count: 1 }
            && GuestModuleValidator.Validate(mixedModule).Succeeded
            && WasmModuleCompiler.Compile(mixedModule).Succeeded,
            "a direct cancellation route and Task catch route must coexist in executable WASM: "
                + string.Join(" | ", mixedLowered.Diagnostics.Select(item =>
                    item.Code + ":" + item.Message)) + " segments="
                + string.Join(" | ", mixedSemantic.AsyncMethods.Single(method =>
                    method.MethodSymbolId.Contains(".RunAsync(", StringComparison.Ordinal))
                    .Segments.Select(segment => $"{segment.Ordinal}:"
                        + $"{segment.Transfer?.Kind}:"
                        + $"{segment.Transfer?.PrimaryTarget}/"
                        + $"{segment.Transfer?.SecondaryTarget}/"
                        + $"{segment.Transfer?.CancellationTarget}")));
        foreach (string variant in new[]
        {
            mixedSource.Replace("if (fail) throw new InvalidOperationException();", ""),
            mixedSource.Replace("throw new InvalidOperationException();", "throw new ArgumentException();"),
            mixedSource.Replace("catch (InvalidOperationException)", "catch (Exception)"),
        })
        {
            FrontendDocument variantFrontend = FrontendAnalyzer.Analyze(variant, mixedSourceId);
            SemanticDocument variantSemantic = SemanticAnalyzer.Analyze(variant, mixedSourceId,
                variantFrontend.Source.Sha256,
                new[] { new SemanticReferenceSource(
                    CSharpGuestContinuationTests.ReferenceFacade + additionalFacade,
                    "generated://AvidScript.Continuations.generated.cs", true) },
                new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
                enableDirectAwaitCleanup: true);
            CSharpGuestLoweringResult variantLowered = CSharpGuestLowerer.Lower(
                variantSemantic, new string('c', 64), enableAsyncLanguageErrors: true);
            Check(variantLowered.Succeeded && variantLowered.Module is { } variantModule
                && GuestModuleValidator.Validate(variantModule).Succeeded
                && WasmModuleCompiler.Compile(variantModule).Succeeded,
                "catches must compile with absent, unrelated or derived throw types: "
                    + string.Join(" | ", variantLowered.Diagnostics.Select(item =>
                        item.Code + ":" + item.Message)));
        }
        return 5;
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

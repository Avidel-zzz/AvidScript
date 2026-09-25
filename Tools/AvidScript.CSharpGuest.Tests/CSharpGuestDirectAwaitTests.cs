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
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int CleanupCount;
                public static int Result;
                public static async Task<int> RunAsync()
                {
                    try { await AvidContinuations.NextTickAsync(); return 16; }
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
        return 1;
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

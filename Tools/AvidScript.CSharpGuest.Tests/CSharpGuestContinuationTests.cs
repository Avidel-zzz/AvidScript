using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestContinuationTests
{
    private const string SemanticHash =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

    public static int Run()
    {
        CallbacksProduceOneDeterministicFourCellExport();
        FacadeCallsLowerSharedContinuationImports();
        ExplicitExportConflictFailsClosed();
        TamperedCallbackMetadataFailsClosed();
        return 4;
    }

    private static void FacadeCallsLowerSharedContinuationImports()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidTransient]
                private static AvidContinuation Pending;

                [AvidContinuation(1)]
                public static void Resume() { }

                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void BeginPlay()
                {
                    Pending = AvidContinuations.Delay(0.25f, 1);
                    Pending = AvidContinuations.NextTick(1);
                }

                [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
                public static void EndPlay()
                {
                    Pending.Cancel();
                }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ContinuationFacadeGuest.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("continuation facade calls produced no Guest module");
        GuestImport delay = module.Imports.Single(import => import.Name == "continuation_delay");
        GuestImport cancel = module.Imports.Single(import => import.Name == "continuation_cancel");

        Assert(result.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "generated continuation facade calls should lower to valid Guest IR");
        Assert(delay.Module == "env"
            && delay.ParameterTypeIds.SequenceEqual(new[] { "type:float32", "type:int32" })
            && delay.ReturnTypeId == "type:int64",
            "continuation delay should retain the shared (f32,i32)->i64 ABI");
        Assert(cancel.Module == "env"
            && cancel.ParameterTypeIds.SequenceEqual(new[] { "type:int64" })
            && cancel.ReturnTypeId == "type:int32",
            "continuation cancel should retain the shared i64->i32 ABI");
        Assert(module.Exports.Count(export => export.Name == "avid_on_continuation") == 1
            && WasmModuleCompiler.Compile(module).Succeeded,
            "facade calls should preserve exactly one compiler-owned continuation export in WASM");
    }

    private static void CallbacksProduceOneDeterministicFourCellExport()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(20)]
                public static void Later() { }

                [AvidContinuation(3)]
                public static void Earlier() { }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ContinuationGuest.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("continuation callbacks produced no Guest module");
        GuestExport export = module.Exports.Single();
        GuestFunction router = module.Functions.Single(function => function.Id == export.FunctionId);
        string[] parameterStorage = router.Parameters
            .Select(parameter => module.Types.Single(type => type.Id == parameter.TypeId).Storage)
            .ToArray();
        int abiCells = parameterStorage.Sum(storage => storage == "i64" ? 2 : 1);
        string[] callTargets = router.Blocks
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call")
            .Select(instruction => instruction.TargetId!)
            .ToArray();
        string[] expectedTargets = document.ContinuationCallbacks
            .OrderBy(callback => callback.CallbackId)
            .Select(callback => "function:" + callback.MethodSymbolId)
            .ToArray();
        string tokenId = router.Parameters[1].Id;
        string statusId = router.Parameters[2].Id;

        Assert(result.Succeeded
            && export.Name == "avid_on_continuation"
            && parameterStorage.SequenceEqual(new[] { "i32", "i64", "i32" })
            && abiCells == 4,
            "continuations should generate exactly one avid_on_continuation i32/i64/i32 four-cell export");
        Assert(router.EntryBlockId.EndsWith(":check:3", StringComparison.Ordinal)
            && callTargets.SequenceEqual(expectedTargets),
            "continuation dispatch should follow ascending callback ids deterministically");
        Assert(router.Blocks.SelectMany(block => block.Instructions).All(instruction =>
                !instruction.OperandIds.Contains(tokenId, StringComparer.Ordinal)
                && !instruction.OperandIds.Contains(statusId, StringComparer.Ordinal))
            && router.Blocks.All(block => block.Terminator.ConditionValueId != tokenId
                && block.Terminator.ConditionValueId != statusId
                && block.Terminator.ReturnValueId != tokenId
                && block.Terminator.ReturnValueId != statusId),
            "C1 dispatch should ignore the reserved token and status parameters");
        Assert(GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "continuation Guest IR should validate and compile into WASM");
    }

    private static void ExplicitExportConflictFailsClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(1)]
                public static void Resume() { }

                [UnmanagedCallersOnly(EntryPoint = "avid_on_continuation")]
                public static void ConflictingExport(int callbackId, long token, int status) { }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ContinuationConflict.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1009"
                && diagnostic.Message.Contains("conflicts", StringComparison.Ordinal)),
            "an explicit avid_on_continuation export should fail closed");
    }

    private static void TamperedCallbackMetadataFailsClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(1)]
                public static void Resume() { }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/TamperedContinuation.cs");
        SemanticContinuationCallback callback = document.ContinuationCallbacks.Single();
        SemanticDocument tampered = document with
        {
            ContinuationCallbacks = new[] { callback with { CallbackId = 0 } },
        };
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(tampered, SemanticHash);

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "guest input validation should reject tampered continuation callback ids");
    }

    private static SemanticDocument Analyze(string source, string sourceId)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[]
            {
                new SemanticReferenceSource(
                    ContinuationFacade,
                    "generated://AvidScript.Continuations.generated.cs",
                    true),
            });
        Assert(document.Succeeded, "continuation source should produce a valid semantic artifact");
        return document;
    }

    private const string ContinuationFacade = """
        using System;
        using System.Runtime.InteropServices;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidContinuationAttribute : Attribute
        {
            public AvidContinuationAttribute(int callbackId) { }
        }

        [AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = false)]
        public sealed class AvidTransientAttribute : Attribute { }

        public readonly struct AvidContinuation
        {
            private readonly long Token;
            internal AvidContinuation(long token) { Token = token; }
            public bool Cancel() => AvidScriptRuntimeNative.ContinuationCancel(Token) != 0;
        }

        public static class AvidContinuations
        {
            public static AvidContinuation Delay(float delaySeconds, int callbackId)
                => new(AvidScriptRuntimeNative.ContinuationDelay(delaySeconds, callbackId));

            public static AvidContinuation NextTick(int callbackId)
                => Delay(0.0f, callbackId);
        }

        internal static class AvidScriptRuntimeNative
        {
            [DllImport("env", EntryPoint = "continuation_delay")]
            internal static extern long ContinuationDelay(float delaySeconds, int callbackId);

            [DllImport("env", EntryPoint = "continuation_cancel")]
            internal static extern int ContinuationCancel(long token);
        }
        """;

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

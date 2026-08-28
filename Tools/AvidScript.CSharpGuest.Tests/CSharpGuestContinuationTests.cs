using System;
using System.Linq;
using System.Text.Json;
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
        CallbacksProduceOneDeterministicSixCellV2Export();
        FacadeCallsLowerSharedContinuationImports();
        SchemaTenRetainsLegacyRouterAndDefaultPayload();
        ExplicitExportConflictFailsClosed();
        TamperedCallbackMetadataFailsClosed();
        return 5;
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

                [AvidContinuation(2)]
                public static void ObjectLoaded(
                    AvidContinuationStatus status,
                    AvidLoadedObject loadedObject) { }

                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void BeginPlay()
                {
                    Pending = AvidContinuations.Delay(0.25f, 1);
                    Pending = AvidContinuations.NextTick(1);
                    Pending = AvidAssets.LoadObjectAsync(
                        "/Engine/EngineMeshes/Cube.Cube",
                        2);
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
        GuestImport loadObject = module.Imports.Single(import =>
            import.Name == "continuation_load_object");
        GuestExport continuationExport = module.Exports.Single(export =>
            export.Name == "avid_on_continuation_v2");
        GuestFunction router = module.Functions.Single(function =>
            function.Id == continuationExport.FunctionId);
        GuestBasicBlock objectCall = router.Blocks.Single(block =>
            block.Id.EndsWith(":call:2", StringComparison.Ordinal));

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
        Assert(loadObject.Module == "env"
            && loadObject.ParameterTypeIds.SequenceEqual(new[] { "type:string", "type:int32" })
            && loadObject.ReturnTypeId == "type:int64",
            "object loading should lower the shared (utf8-ref,i32)->i64 import");
        Assert(objectCall.Instructions.Any(instruction =>
                instruction.Op == "convert"
                && router.Locals.Single(local => local.Id == instruction.ResultId).TypeId
                    == "type:global::AvidScript.AvidContinuationStatus")
            && objectCall.Instructions.Count(instruction => instruction.Op == "field_store") == 2
            && objectCall.Instructions.Any(instruction =>
                instruction.Op == "field_store"
                && instruction.TargetId?.EndsWith(".Slot:int32", StringComparison.Ordinal) == true)
            && objectCall.Instructions.Any(instruction =>
                instruction.Op == "field_store"
                && instruction.TargetId?.EndsWith(".Generation:int32", StringComparison.Ordinal) == true)
            && objectCall.Instructions.Single(instruction => instruction.Op == "call").OperandIds.Count == 2,
            "v2 dispatch should construct status and loaded-object payload values for object handlers");
        Assert(module.Exports.Count(export => export.Name == "avid_on_continuation_v2") == 1
            && module.Exports.All(export => export.Name != "avid_on_continuation")
            && WasmModuleCompiler.Compile(module).Succeeded,
            "schema 11 facade calls should preserve exactly one compiler-owned v2 continuation export in WASM");
    }

    private static void CallbacksProduceOneDeterministicSixCellV2Export()
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
        string[] reservedParameterIds = router.Parameters
            .Skip(1)
            .Select(parameter => parameter.Id)
            .ToArray();

        Assert(result.Succeeded
            && export.Name == "avid_on_continuation_v2"
            && router.Id == "function:synthetic:continuation_v2"
            && router.Parameters.All(parameter => parameter.Id.StartsWith(
                "value:continuation_v2:parameter:",
                StringComparison.Ordinal))
            && parameterStorage.SequenceEqual(new[] { "i32", "i64", "i32", "i32", "i32" })
            && abiCells == 6,
            "schema 11 continuations should generate one five-parameter avid_on_continuation_v2 export");
        Assert(router.EntryBlockId.EndsWith(":check:3", StringComparison.Ordinal)
            && callTargets.SequenceEqual(expectedTargets),
            "continuation dispatch should follow ascending callback ids deterministically");
        Assert(router.Blocks.SelectMany(block => block.Instructions).All(instruction =>
                reservedParameterIds.All(parameterId =>
                    !instruction.OperandIds.Contains(parameterId, StringComparer.Ordinal)))
            && router.Blocks.All(block => reservedParameterIds.All(parameterId =>
                block.Terminator.ConditionValueId != parameterId
                && block.Terminator.ReturnValueId != parameterId)),
            "zero-payload v2 dispatch should ignore token, status, slot, and generation parameters");
        Assert(GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "continuation Guest IR should validate and compile into WASM");
    }

    private static void SchemaTenRetainsLegacyRouterAndDefaultPayload()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(7)]
                public static void Resume() { }
            }
            """;
        SemanticDocument current = Analyze(source, "Scripts/LegacyContinuationGuest.cs");
        SemanticContinuationCallback legacyJsonCallback = JsonSerializer.Deserialize<SemanticContinuationCallback>(
            """
            {
              "callback_id": 7,
              "name": "Resume",
              "method_symbol_id": "legacy",
              "span": { "start": 0, "length": 6, "line": 0, "column": 0, "end_line": 0, "end_column": 6 }
            }
            """,
            new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower })
            ?? throw new InvalidOperationException("legacy callback JSON produced null");
        SemanticDocument schemaTen = current with
        {
            SchemaVersion = 10,
            SemanticVersion = "1.10",
        };
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(schemaTen, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("schema 10 continuation callbacks produced no Guest module");
        GuestExport export = module.Exports.Single(export => export.Name == "avid_on_continuation");
        GuestFunction router = module.Functions.Single(function => function.Id == export.FunctionId);

        Assert(legacyJsonCallback.PayloadKind == "none",
            "schema 10 callback JSON without payload_kind should default to none");
        Assert(result.Succeeded
            && router.Id == "function:synthetic:continuation"
            && router.Parameters.All(parameter => parameter.Id.StartsWith(
                "value:continuation:parameter:",
                StringComparison.Ordinal))
            && router.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32", "type:int64", "type:int32" })
            && module.Exports.All(item => item.Name != "avid_on_continuation_v2")
            && GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "schema 10 / 1.10 should retain the legacy three-parameter continuation router");
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

                [UnmanagedCallersOnly(EntryPoint = "avid_on_continuation_v2")]
                public static void ConflictingExport(
                    int callbackId,
                    long token,
                    int status,
                    int objectSlot,
                    int objectGeneration) { }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/ContinuationConflict.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1009"
                && diagnostic.Message.Contains("conflicts", StringComparison.Ordinal)),
            "an explicit avid_on_continuation_v2 export should fail closed");
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
        SemanticDocument tamperedPayload = document with
        {
            ContinuationCallbacks = new[] { callback with { PayloadKind = "object" } },
        };
        CSharpGuestLoweringResult payloadResult = CSharpGuestLowerer.Lower(
            tamperedPayload,
            SemanticHash);
        SemanticDocument legacyObjectPayload = tamperedPayload with
        {
            SchemaVersion = 10,
            SemanticVersion = "1.10",
        };
        CSharpGuestLoweringResult legacyPayloadResult = CSharpGuestLowerer.Lower(
            legacyObjectPayload,
            SemanticHash);

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "guest input validation should reject tampered continuation callback ids");
        Assert(!payloadResult.Succeeded
            && payloadResult.Module is null
            && payloadResult.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001")
            && !legacyPayloadResult.Succeeded
            && legacyPayloadResult.Module is null
            && legacyPayloadResult.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "guest input validation should reject mismatched and schema 10 object payload metadata");
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

        public enum AvidContinuationStatus : int
        {
            Completed = 1,
            Failed = 2,
        }

        public readonly struct AvidLoadedObject
        {
            internal readonly int Slot;
            internal readonly int Generation;
            internal AvidLoadedObject(int slot, int generation)
            {
                Slot = slot;
                Generation = generation;
            }
        }

        public static class AvidAssets
        {
            public static AvidContinuation LoadObjectAsync(string assetPath, int callbackId)
                => new(AvidScriptRuntimeNative.ContinuationLoadObject(assetPath, callbackId));
        }

        internal static class AvidScriptRuntimeNative
        {
            [DllImport("env", EntryPoint = "continuation_delay")]
            internal static extern long ContinuationDelay(float delaySeconds, int callbackId);

            [DllImport("env", EntryPoint = "continuation_cancel")]
            internal static extern int ContinuationCancel(long token);

            [DllImport("env", EntryPoint = "continuation_load_object")]
            internal static extern long ContinuationLoadObject(string assetPath, int callbackId);
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

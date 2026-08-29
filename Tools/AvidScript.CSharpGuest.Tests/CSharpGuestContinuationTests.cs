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
        AsyncAwaitLowersZeroHeapResumeSegments();
        GeneratedLatentAwaitLowersDynamicImport();
        GeneratedLatentEnumAwaitUsesUnderlyingStorage();
        GeneratedLatentAggregatesUseSharedStoragePlan();
        CancellationMarkerBindsScheduledContinuation();
        return 10;
    }

    private static void CancellationMarkerBindsScheduledContinuation()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private static AvidCancellationSource Cancellation;

                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    Cancellation = AvidCancellationSource.Create();
                    await AvidContinuations.DelayAsync(0.25f)
                        .WithCancellation(Cancellation.Token);
                }

                [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
                public static void EndPlay()
                {
                    Cancellation.Cancel();
                    Cancellation.Release();
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/CancellationMarkerGuest.cs");
        SemanticAsyncAwaitSite awaitSite = document.AsyncMethods.Single()
            .Segments.Single(segment => segment.AwaitSite is not null)
            .AwaitSite!;
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(diagnostic => diagnostic.Message)));
        GuestImport delayImport = module.Imports.Single(import =>
            import.Name == "continuation_delay");
        GuestImport bindImport = module.Imports.Single(import =>
            import.Name == "continuation_bind_cancel");
        GuestImport createImport = module.Imports.Single(import =>
            import.Name == "continuation_cancel_source_create");
        GuestImport cancelImport = module.Imports.Single(import =>
            import.Name == "continuation_cancel_source_cancel");
        GuestImport releaseImport = module.Imports.Single(import =>
            import.Name == "continuation_cancel_source_release");
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        int delayCallIndex = Array.FindIndex(instructions, instruction =>
            instruction.Op == "call" && instruction.TargetId == delayImport.Id);
        int bindCallIndex = Array.FindIndex(instructions, instruction =>
            instruction.Op == "call" && instruction.TargetId == bindImport.Id);
        int cancellationLoadIndex = Array.FindIndex(instructions, instruction =>
            instruction.Op == "field_load");
        GuestInstruction bindCall = instructions[bindCallIndex];

        Assert(awaitSite.CancellationToken?.TypeId
                == "type:global::AvidScript.AvidCancellationToken"
            && result.Succeeded
            && bindImport.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:int64", "type:int64",
            })
            && bindImport.ReturnTypeId == "type:int32"
            && createImport.ParameterTypeIds.Count == 0
            && createImport.ReturnTypeId == "type:int64"
            && cancelImport.ParameterTypeIds.SequenceEqual(new[] { "type:int64" })
            && cancelImport.ReturnTypeId == "type:int32"
            && releaseImport.ParameterTypeIds.SequenceEqual(new[] { "type:int64" })
            && releaseImport.ReturnTypeId == "type:int32"
            && cancellationLoadIndex >= 0
            && delayCallIndex > cancellationLoadIndex
            && bindCallIndex > delayCallIndex
            && bindCall.OperandIds.Count == 2
            && bindCall.OperandIds[1] == instructions[delayCallIndex].ResultId
            && instructions.Any(instruction => instruction.Op == "field_load")
            && instructions.Any(instruction =>
                instruction.Op == "binary" && instruction.OperatorKind == "bitwise_and")
            && instructions.Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == createImport.Id)
            && instructions.Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == cancelImport.Id)
            && instructions.Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == releaseImport.Id)
            && GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "script-owned cancellation sources should create, bind, cancel, and release through compiler-owned lowering");
    }

    private static void GeneratedLatentAggregatesUseSharedStoragePlan()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    await UKismetSystemLibrary.WaitForTargetAsync(default);
                    await UKismetSystemLibrary.WaitForLocationAsync(
                        new FVector(1.0f, 2.0f, 3.0f));
                    await UKismetSystemLibrary.WaitForSettingsAsync(default);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/GeneratedLatentAggregateGuest.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                result.Diagnostics.Select(diagnostic => diagnostic.Message)));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        GuestImport objectImport = module.Imports.Single(import =>
            import.Name == "avid_ue_latent_object_test");
        GuestImport vectorImport = module.Imports.Single(import =>
            import.Name == "avid_ue_latent_vector_test");
        GuestImport wireImport = module.Imports.Single(import =>
            import.Name == "avid_ue_latent_wire_test");
        GuestInstruction objectCall = instructions.Single(instruction =>
            instruction.Op == "call" && instruction.TargetId == objectImport.Id);
        GuestInstruction vectorCall = instructions.Single(instruction =>
            instruction.Op == "call" && instruction.TargetId == vectorImport.Id);
        GuestInstruction wireCall = instructions.Single(instruction =>
            instruction.Op == "call" && instruction.TargetId == wireImport.Id);

        Assert(result.Succeeded
            && objectImport.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:int32", "type:int32", "type:int32",
            })
            && vectorImport.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:float32", "type:float32", "type:float32", "type:int32",
            })
            && wireImport.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:address", "type:int32",
            })
            && objectCall.OperandIds.Count == 3
            && vectorCall.OperandIds.Count == 4
            && wireCall.OperandIds.Count == 2
            && instructions.Count(instruction => instruction.Op == "field_load") >= 5
            && instructions.Any(instruction => instruction.Op == "address_of")
            && GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "generated latent aggregates should share recursive field and address storage plans");
    }

    private static void GeneratedLatentEnumAwaitUsesUnderlyingStorage()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    await UKismetSystemLibrary.WaitForModeAsync();
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/GeneratedLatentEnumGuest.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("generated latent enum await produced no Guest module");
        GuestImport latentImport = module.Imports.Single(import =>
            import.Module == "avidscript" && import.Name == "avid_ue_latent_enum_test");
        GuestFunction beginPlay = module.Functions.Single(function =>
            function.Id == module.Exports.Single(export =>
                export.Name == "avid_on_begin_play").FunctionId);
        GuestInstruction producerCall = beginPlay.Blocks
            .SelectMany(block => block.Instructions)
            .Single(instruction => instruction.Op == "call"
                && instruction.TargetId == latentImport.Id);
        GuestInstruction storageConversion = beginPlay.Blocks
            .SelectMany(block => block.Instructions)
            .Single(instruction => instruction.Op == "convert"
                && instruction.ResultId == producerCall.OperandIds[0]);
        GuestRegister[] registers = beginPlay.Parameters.Concat(beginPlay.Locals).ToArray();
        GuestRegister sourceRegister = registers.Single(register =>
            register.Id == storageConversion.OperandIds[0]);
        GuestRegister storageRegister = registers.Single(register =>
            register.Id == storageConversion.ResultId);

        Assert(result.Succeeded
            && latentImport.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:int32",
                "type:int32",
            })
            && sourceRegister.TypeId.EndsWith(
                "EAvidScriptCSharpEmitterTestMode",
                StringComparison.Ordinal)
            && storageRegister.TypeId == "type:int32"
            && GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "generated latent enum defaults should lower through the declared enum underlying storage");
    }

    private static void GeneratedLatentAwaitLowersDynamicImport()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    await UKismetSystemLibrary.WaitForFlagAsync(true);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/GeneratedLatentGuest.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("generated latent await produced no Guest module");
        GuestImport latentImport = module.Imports.Single(import =>
            import.Module == "avidscript" && import.Name == "avid_ue_latent_bool_test");
        GuestFunction beginPlay = module.Functions.Single(function =>
            function.Id == module.Exports.Single(export =>
                export.Name == "avid_on_begin_play").FunctionId);
        GuestInstruction producerCall = beginPlay.Blocks
            .SelectMany(block => block.Instructions)
            .Single(instruction => instruction.Op == "call"
                && instruction.TargetId == latentImport.Id);

        Assert(result.Succeeded
            && latentImport.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:int32",
                "type:int32",
            })
            && latentImport.ReturnTypeId == "type:int64"
            && producerCall.OperandIds.Count == 2
            && beginPlay.Blocks.SelectMany(block => block.Instructions)
                .Any(instruction => instruction.Op == "convert")
            && module.Imports.All(import => import.Name != "continuation_delay"
                && import.Name != "continuation_load_object"),
            "generated latent await should use only its descriptor-driven dynamic import and compiler callback");
        Assert(GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "generated latent await Guest IR should validate and compile to WASM");
    }

    private static void AsyncAwaitLowersZeroHeapResumeSegments()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    await AvidContinuations.NextTickAsync();
                    AvidLoadedObject loaded = await AvidAssets.LoadObjectAsync(
                        "/Engine/EngineMeshes/Cube.Cube");
                    Consume(loaded);
                    await AvidContinuations.DelayAsync(0.25f);
                }

                private static void Consume(AvidLoadedObject loaded) { }
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/AsyncAwaitGuest.cs");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("async await source produced no Guest module");
        SemanticAsyncMethod asyncMethod = document.AsyncMethods.Single();
        int[] callbackIds = asyncMethod.Segments
            .Where(segment => segment.AwaitSite is not null)
            .Select(segment => segment.AwaitSite!.CallbackId)
            .ToArray();
        GuestFunction[] resumeFunctions = module.Functions
            .Where(function => function.Id.StartsWith(
                "function:synthetic:async_resume:",
                StringComparison.Ordinal))
            .OrderBy(function => function.Id, StringComparer.Ordinal)
            .ToArray();
        GuestFunction router = module.Functions.Single(function =>
            function.Id == "function:synthetic:continuation_v2");
        CSharpGuestDebugMap debugMap = CSharpGuestDebugMapProjector.Project(
            document,
            module,
            SemanticHash,
            document.Source.FrontendSha256);
        CSharpGuestDebugFunction[] resumeDebugFunctions = debugMap.Functions
            .Where(function => function.GuestFunctionId.StartsWith(
                "function:synthetic:async_resume:",
                StringComparison.Ordinal))
            .OrderBy(function => function.WasmFunctionIndex)
            .ToArray();
        string[] routeTargets = router.Blocks
            .SelectMany(block => block.Instructions)
            .Where(instruction => instruction.Op == "call"
                && instruction.TargetId?.StartsWith(
                    "function:synthetic:async_resume:",
                    StringComparison.Ordinal) == true)
            .Select(instruction => instruction.TargetId!)
            .ToArray();

        Assert(result.Succeeded
            && document.SchemaVersion == 12
            && document.SemanticVersion == "1.12"
            && asyncMethod.Lowering == "reentrant_zero_heap_cps"
            && callbackIds.SequenceEqual(new[]
            {
                0x40000000,
                0x40000001,
                0x40000002,
            }),
            "semantic async methods should publish deterministic compiler-owned await sites");
        Assert(module.Exports.Any(export => export.Name == "avid_on_begin_play")
            && module.Exports.Any(export => export.Name == "avid_on_continuation_v2")
            && resumeFunctions.Length == 3
            && routeTargets.SequenceEqual(callbackIds.Select(id =>
                $"function:synthetic:async_resume:{id}")),
            "async await should lower to one exported entry and deterministic resume routes");
        Assert(module.Imports.Count(import => import.Name == "continuation_delay") == 1
            && module.Imports.Count(import => import.Name == "continuation_load_object") == 1
            && resumeFunctions.Single(function => function.Id.EndsWith(
                callbackIds[1].ToString(),
                StringComparison.Ordinal)).Parameters.Count == 2,
            "object await resume should consume the v2 status/object payload without a new Host ABI");
        Assert(module.Functions
                .Where(function => function.Id == module.Exports.Single(export =>
                        export.Name == "avid_on_begin_play").FunctionId
                    || function.Id.StartsWith(
                        "function:synthetic:async_resume:",
                        StringComparison.Ordinal))
                .SelectMany(function => function.Blocks)
                .Count(block => block.Terminator.Kind == "trap") == 3,
            "each await producer should trap immediately when the Session rejects scheduling");
        Assert(resumeDebugFunctions.Length == 3
            && resumeDebugFunctions.Select(function => function.MethodSymbolId).Distinct().Count() == 3
            && resumeDebugFunctions.All(function => function.MethodSymbolId.Contains(
                "#async_resume:",
                StringComparison.Ordinal))
            && resumeDebugFunctions.Select(function => function.Span)
                .SequenceEqual(asyncMethod.Segments.Skip(1).Select(segment => segment.Span)),
            "each generated resume function should map back to its source async segment");
        Assert(GuestModuleValidator.Validate(module).Succeeded
            && WasmModuleCompiler.Compile(module).Succeeded,
            "zero-heap async Guest IR should validate and compile to WASM");
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
        using System.Runtime.CompilerServices;
        using System.Runtime.InteropServices;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidContinuationAttribute : Attribute
        {
            public AvidContinuationAttribute(int callbackId) { }
        }

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidLatentAttribute : Attribute
        {
            public AvidLatentAttribute(string module, string importName) { }
        }

        [AttributeUsage(AttributeTargets.Field, Inherited = false, AllowMultiple = false)]
        public sealed class AvidTransientAttribute : Attribute { }

        public readonly struct AvidContinuation
        {
            private readonly long Token;
            internal AvidContinuation(long token) { Token = token; }
            public bool Cancel() => AvidScriptRuntimeNative.ContinuationCancel(Token) != 0;
        }

        public readonly struct AvidDelayAwaitable
        {
            private readonly int Marker;
            public AvidDelayAwaitable WithCancellation(AvidCancellationToken token) => default;
            public AvidDelayAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidDelayAwaiter : INotifyCompletion
        {
            private readonly int Marker;
            public bool IsCompleted => false;
            public void OnCompleted(Action continuation) { }
            public void GetResult() { }
        }

        public readonly struct AvidObjectAwaitable
        {
            private readonly int Marker;
            public AvidObjectAwaitable WithCancellation(AvidCancellationToken token) => default;
            public AvidObjectAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidObjectAwaiter : INotifyCompletion
        {
            private readonly int Marker;
            public bool IsCompleted => false;
            public void OnCompleted(Action continuation) { }
            public AvidLoadedObject GetResult() => default;
        }

        public static class AvidContinuations
        {
            public static AvidContinuation Delay(float delaySeconds, int callbackId)
                => new(AvidScriptRuntimeNative.ContinuationDelay(delaySeconds, callbackId));

            public static AvidContinuation NextTick(int callbackId)
                => Delay(0.0f, callbackId);

            public static AvidDelayAwaitable DelayAsync(float delaySeconds) => default;
            public static AvidDelayAwaitable NextTickAsync() => default;
        }

        public enum AvidContinuationStatus : int
        {
            Completed = 1,
            Failed = 2,
            Cancelled = 3,
        }

        public readonly struct AvidCancellationToken
        {
            internal readonly long Value;
            internal AvidCancellationToken(long value) { Value = value; }
        }

        public readonly struct AvidCancellationSource
        {
            private readonly long Value;
            public readonly AvidCancellationToken Token;
            private AvidCancellationSource(long value)
            {
                Value = value;
                Token = new AvidCancellationToken(value);
            }
            public static AvidCancellationSource Create()
                => new(AvidScriptRuntimeNative.ContinuationCancelSourceCreate());
            public bool Cancel()
                => AvidScriptRuntimeNative.ContinuationCancelSourceCancel(Value) != 0;
            public bool Release()
                => AvidScriptRuntimeNative.ContinuationCancelSourceRelease(Value) != 0;
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

            public static AvidObjectAwaitable LoadObjectAsync(string assetPath) => default;
        }

        public static class UKismetSystemLibrary
        {
            [AvidLatent("avidscript", "avid_ue_latent_test")]
            public static AvidDelayAwaitable DelayAsync(float Duration) => default;

            [AvidLatent("avidscript", "avid_ue_latent_bool_test")]
            public static AvidDelayAwaitable WaitForFlagAsync(bool bExpected) => default;

            [AvidLatent("avidscript", "avid_ue_latent_enum_test")]
            public static AvidDelayAwaitable WaitForModeAsync(
                EAvidScriptCSharpEmitterTestMode Mode = EAvidScriptCSharpEmitterTestMode.Primary) => default;

            [AvidLatent("avidscript", "avid_ue_latent_object_test")]
            public static AvidDelayAwaitable WaitForTargetAsync(UObject Target) => default;

            [AvidLatent("avidscript", "avid_ue_latent_vector_test")]
            public static AvidDelayAwaitable WaitForLocationAsync(FVector Location) => default;

            [AvidLatent("avidscript", "avid_ue_latent_wire_test")]
            public static AvidDelayAwaitable WaitForSettingsAsync(
                FAvidScriptStructWireRootTestType Settings) => default;
        }

        public enum EAvidScriptCSharpEmitterTestMode : int
        {
            Primary,
            Secondary,
        }

        public readonly struct UObject
        {
            private readonly int Slot;
            private readonly int Generation;
            internal int AvidScriptSlot => Slot;
            internal int AvidScriptGeneration => Generation;
        }

        public readonly struct FVector
        {
            public readonly float X;
            public readonly float Y;
            public readonly float Z;
            public FVector(float x, float y, float z)
            {
                X = x;
                Y = y;
                Z = z;
            }
        }

        public struct FAvidScriptStructWireRootTestType
        {
            public int Count;
        }

        internal static class AvidScriptRuntimeNative
        {
            [DllImport("env", EntryPoint = "continuation_delay")]
            internal static extern long ContinuationDelay(float delaySeconds, int callbackId);

            [DllImport("env", EntryPoint = "continuation_cancel")]
            internal static extern int ContinuationCancel(long token);

            [DllImport("env", EntryPoint = "continuation_load_object")]
            internal static extern long ContinuationLoadObject(string assetPath, int callbackId);

            [DllImport("env", EntryPoint = "continuation_cancel_source_create")]
            internal static extern long ContinuationCancelSourceCreate();

            [DllImport("env", EntryPoint = "continuation_cancel_source_cancel")]
            internal static extern int ContinuationCancelSourceCancel(long sourceToken);

            [DllImport("env", EntryPoint = "continuation_cancel_source_release")]
            internal static extern int ContinuationCancelSourceRelease(long sourceToken);

            [DllImport("env", EntryPoint = "continuation_bind_cancel")]
            internal static extern int ContinuationBindCancel(
                long sourceToken,
                long continuationToken);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_test")]
            internal static extern long InvokeLatent(float duration, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_bool_test")]
            internal static extern long InvokeBooleanLatent(int expected, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_enum_test")]
            internal static extern long InvokeEnumLatent(int mode, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_object_test")]
            internal static extern long InvokeObjectLatent(int slot, int generation, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_vector_test")]
            internal static extern long InvokeVectorLatent(float x, float y, float z, int callbackId);

            [DllImport("avidscript", EntryPoint = "avid_ue_latent_wire_test")]
            internal static extern long InvokeWireLatent(
                in FAvidScriptStructWireRootTestType settings,
                int callbackId);
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

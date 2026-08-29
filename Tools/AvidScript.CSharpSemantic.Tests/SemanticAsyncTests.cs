using System;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticAsyncTests
{
    private const int CompilerCallbackIdStart = 0x40000000;

    public static int Run()
    {
        SequentialAwaitsProjectStableSegments();
        EarlyReturnGuardsProjectStableControlOperations();
        CrossBoundaryLocalsPublishStateFrames();
        StructuredFlowProjectsExactStateFrames();
        NestedAwaitsProjectContinuationCfg();
        SwitchAwaitsProjectContinuationCfg();
        NonFrozenAsyncShapesRemainFailClosed();
        CallbackRangesAndAwaitLimitsAreEnforced();
        ControlFlowSegmentLimitFailsClosed();
        GeneratedLatentProducerProjectsImportIdentity();
        return 10;
    }

    private static void EarlyReturnGuardsProjectStableControlOperations()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("guarded")]
                public static async void Guarded()
                {
                    await AvidContinuations.NextTickAsync();
                    if (ShouldStop())
                    {
                        return;
                    }
                    await AvidContinuations.NextTickAsync();
                }

                private static bool ShouldStop() => false;
            }
            """;
        SemanticDocument document = Analyze(source, "Scripts/GuardedAsync.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncStatement guard = method.Segments[1].Statements.Single();

        Assert(document.Succeeded
            && document.SchemaVersion == 15
            && document.SemanticVersion == "1.17"
            && method.Segments.Count == 3
            && guard.TargetSymbolId is null
            && guard.Operation.Kind == SemanticAsyncMethod.EarlyReturnGuardOperationKind
            && guard.Operation.TypeId == "type:void"
            && guard.Operation.Children.Count == 1
            && guard.Operation.Children[0].TypeId == "type:bool",
            "top-level early-return guards should remain explicit under the schema-v15 semantic-1.17 contract");

        const string invalidSource = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("invalid_guard")]
                public static async void InvalidGuard()
                {
                    await AvidContinuations.NextTickAsync();
                    if (ShouldStop())
                    {
                        Consume();
                    }
                    else
                    {
                        Consume();
                    }
                }

                private static bool ShouldStop() => false;
                private static void Consume() { }
            }
            """;
        SemanticDocument branched = Analyze(invalidSource, "Scripts/BranchedAsync.cs");
        SemanticOperation branch = branched.AsyncMethods.Single()
            .Segments[1]
            .Statements.Single()
            .Operation;
        Assert(branched.Succeeded
            && branch.Kind == SemanticAsyncMethod.IfOperationKind
            && branch.Children.Count == 3
            && branch.Children.Skip(1).All(child =>
                child.Kind == SemanticAsyncMethod.BlockOperationKind),
            "if/else side effects should project as explicit structured async flow");
    }

    private static void GeneratedLatentProducerProjectsImportIdentity()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    await UKismetSystemLibrary.DelayAsync(0.125f);
                    await UKismetSystemLibrary.WaitForFlagAsync(true);
                    await UKismetSystemLibrary.WaitForModeAsync();
                    await UKismetSystemLibrary.WaitForTargetAsync(default);
                    await UKismetSystemLibrary.WaitForLocationAsync(new FVector(1.0f, 2.0f, 3.0f));
                    await UKismetSystemLibrary.WaitForSettingsAsync(default);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/GeneratedLatentAsync.cs");
        SemanticAsyncAwaitSite[] sites = document.AsyncMethods.Single()
            .Segments.Where(segment => segment.AwaitSite is not null)
            .Select(segment => segment.AwaitSite!)
            .ToArray();
        SemanticAsyncAwaitSite delaySite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_test");
        SemanticAsyncAwaitSite boolSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_bool_test");
        SemanticAsyncAwaitSite enumSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_enum_test");
        SemanticAsyncAwaitSite objectSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_object_test");
        SemanticAsyncAwaitSite vectorSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_vector_test");
        SemanticAsyncAwaitSite wireSite = sites.Single(site =>
            site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_wire_test");
        SemanticCallable delayImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_test" });
        SemanticCallable boolImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_bool_test" });
        SemanticCallable enumImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_enum_test" });
        SemanticCallable objectImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_object_test" });
        SemanticCallable vectorImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_vector_test" });
        SemanticCallable wireImport = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_wire_test" });

        Assert(document.Succeeded
            && delaySite.CallbackId == CompilerCallbackIdStart
            && delaySite.PayloadKind == "none"
            && delaySite.Arguments.Count == 1
            && delaySite.Arguments[0].TypeId == "type:float32"
            && delayImport.ReturnTypeId == "type:int64"
            && delayImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:float32", "type:int32" }),
            "generated latent markers should project a generic import identity and compiler callback ABI");
        Assert(boolSite.CallbackId == CompilerCallbackIdStart + 1
            && boolSite.PayloadKind == "none"
            && boolSite.Arguments.Count == 1
            && boolSite.Arguments[0].TypeId == "type:bool"
            && boolImport.ReturnTypeId == "type:int64"
            && boolImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32", "type:int32" }),
            "generated boolean latent producers should preserve public bool and import i32 storage identities");
        Assert(enumSite.CallbackId == CompilerCallbackIdStart + 2
            && enumSite.PayloadKind == "none"
            && enumSite.Arguments.Count == 1
            && enumSite.Arguments[0].TypeId is { } enumArgumentTypeId
            && enumArgumentTypeId.EndsWith(
                "EAvidScriptCSharpEmitterTestMode",
                StringComparison.Ordinal)
            && enumImport.ReturnTypeId == "type:int64"
            && enumImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32", "type:int32" }),
            "generated enum latent producers should materialize the default enum and retain i32 import storage");
        Assert(objectSite.CallbackId == CompilerCallbackIdStart + 3
            && objectSite.Arguments.Count == 1
            && objectSite.Arguments[0].TypeId?.EndsWith("UObject", StringComparison.Ordinal) == true
            && objectImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:int32", "type:int32", "type:int32" }),
            "generated object latent producers should retain one public capability and two import cells");
        Assert(vectorSite.CallbackId == CompilerCallbackIdStart + 4
            && vectorSite.Arguments.Count == 1
            && vectorSite.Arguments[0].TypeId?.EndsWith("FVector", StringComparison.Ordinal) == true
            && vectorImport.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:float32", "type:float32", "type:float32", "type:int32" }),
            "generated vector latent producers should retain one public value and three import cells");
        Assert(wireSite.CallbackId == CompilerCallbackIdStart + 5
            && wireSite.Arguments.Count == 1
            && wireImport.Parameters.Count == 2
            && wireImport.Parameters[0].RefKind == "in"
            && wireImport.Parameters[0].TypeId == wireSite.Arguments[0].TypeId
            && wireImport.Parameters[1].TypeId == "type:int32",
            "generated struct-wire latent producers should retain one public value and one address import");
    }

    private static void SequentialAwaitsProjectStableSegments()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private const string CubePath = "/Engine/EngineMeshes/Cube.Cube";

                [AvidExport("avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    float delay = 0.25f;
                    ConsumeDelay(delay);
                    await AvidContinuations.DelayAsync(GetDelay(delay));
                    NativeAfterAwait(7);
                    await AvidContinuations.NextTickAsync();
                    AvidLoadedObject loaded = await AvidAssets.LoadObjectAsync(CubePath);
                    Consume(loaded);
                }

                [AvidExport("avid_on_end_play")]
                public static async void EndPlay()
                {
                    await AvidContinuations.NextTickAsync();
                }

                private static void ConsumeDelay(float value) { }
                private static float GetDelay(float value) => value;
                private static void Consume(AvidLoadedObject value) { }

                [DllImport("env", EntryPoint = "async_after_await")]
                private static extern void NativeAfterAwait(int value);
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/ControlledAsync.cs");
        SemanticAsyncMethod beginPlay = document.AsyncMethods.Single(method =>
            method.ExportName == "avid_on_begin_play");
        SemanticAsyncMethod endPlay = document.AsyncMethods.Single(method =>
            method.ExportName == "avid_on_end_play");
        SemanticAsyncAwaitSite[] beginAwaits = beginPlay.Segments
            .Select(segment => segment.AwaitSite)
            .Where(site => site is not null)
            .Cast<SemanticAsyncAwaitSite>()
            .ToArray();

        Assert(document.Succeeded
            && document.SchemaVersion == 15
            && document.SemanticVersion == "1.17"
            && document.AsyncMethods.Count == 2,
            "controlled async exports should publish schema 15 / semantic 1.17");
        Assert(beginPlay.Lowering == "reentrant_zero_heap_cps"
            && beginPlay.Segments.Select(segment => segment.Ordinal)
                .SequenceEqual(new[] { 0, 1, 2, 3 })
            && beginAwaits.Select(site => site.CallbackId).SequenceEqual(new[]
            {
                CompilerCallbackIdStart,
                CompilerCallbackIdStart + 1,
                CompilerCallbackIdStart + 2,
            })
            && beginAwaits.Select(site => site.ProducerKind)
                .SequenceEqual(new[] { "delay", "next_tick", "object_load" })
            && beginAwaits.Select(site => site.PayloadKind)
                .SequenceEqual(new[] { "none", "none", "object" }),
            "await sites should preserve source order, producer shape, and compiler callback allocation");
        Assert(beginPlay.Segments[0].Statements.Count == 2
            && beginPlay.Segments[0].Statements[0].TargetSymbolId is not null
            && beginPlay.Segments[0].Statements[0].Operation.Kind == "literal"
            && beginPlay.Segments[3].Statements.Single().Operation.Kind == "expression_statement",
            "ordinary segment statements should project initializer values and optional local targets");
        Assert(beginAwaits[2].ResultSymbolId is not null
            && beginAwaits[2].ResultTypeId == "type:global::AvidScript.AvidLoadedObject"
            && beginAwaits[2].Arguments.Single().Constant?.Value ==
                "/Engine/EngineMeshes/Cube.Cube",
            "object awaits should publish their constant path and immediately resumed result local");
        Assert(endPlay.Segments[0].AwaitSite?.CallbackId == CompilerCallbackIdStart + 3
            && !document.ControlFlowGraphs.Any(graph =>
                graph.MethodSymbolId == beginPlay.MethodSymbolId
                || graph.MethodSymbolId == endPlay.MethodSymbolId),
            "compiler callback ids should continue across exports and async methods should not publish CFGs");
        Assert(document.Reachability?.ReachableImports.Single().Name == "async_after_await"
            && document.Reachability.ReachableCallableIds.Any(id =>
                id.Contains(".Consume(", StringComparison.Ordinal))
            && document.Reachability.ReachableCallableIds.Any(id =>
                id.Contains(".GetDelay(", StringComparison.Ordinal)),
            "resume statements and await arguments should keep direct imports and synchronous callees reachable");
        SemanticOperation asyncRoot = document.Methods.Single(method =>
            method.MethodSymbolId == beginPlay.MethodSymbolId).Root;
        Assert(Enumerate(asyncRoot).Any(operation => operation.Kind == "await"),
            "ordinary operation projection should expose a stable await kind");

        string json = Encoding.UTF8.GetString(SemanticSerializer.Serialize(document));
        Assert(json.Contains("\"async_methods\"", StringComparison.Ordinal)
            && json.Contains("\"result_type_id\": \"type:global::AvidScript.AvidLoadedObject\"", StringComparison.Ordinal)
            && json.IndexOf("\"continuation_callbacks\"", StringComparison.Ordinal)
                < json.IndexOf("\"async_methods\"", StringComparison.Ordinal)
            && json.IndexOf("\"async_methods\"", StringComparison.Ordinal)
                < json.IndexOf("\"diagnostics\"", StringComparison.Ordinal),
            "async graph serialization should be stable and precede diagnostics");
    }

    private static void CrossBoundaryLocalsPublishStateFrames()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("cross_state")]
                public static async void CrossState()
                {
                    int count = 1;
                    FVector offset = new FVector(1.0f, 2.0f, 3.0f);
                    await AvidContinuations.NextTickAsync();
                    AvidLoadedObject loaded = await AvidAssets.LoadObjectAsync("/Game/Valid.Valid");
                    await AvidContinuations.NextTickAsync();
                    Consume(count);
                    Consume(offset);
                    Consume(loaded);
                }

                private static void Consume(int value) { }
                private static void Consume(FVector value) { }
                private static void Consume(AvidLoadedObject value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/CrossAwaitLocals.cs");

        SemanticAsyncAwaitSite[] sites = document.AsyncMethods.Single().Segments
            .Where(segment => segment.AwaitSite is not null)
            .Select(segment => segment.AwaitSite!)
            .ToArray();
        string countId = document.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == "count").Id;
        string offsetId = document.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == "offset").Id;
        string loadedId = document.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == "loaded").Id;

        Assert(document.Succeeded
            && document.AsyncMethods.Count == 1
            && sites.Length == 3
            && sites[0].StateFrame?.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { countId, offsetId }.OrderBy(id => id, StringComparer.Ordinal)) == true
            && sites[1].StateFrame?.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { countId, offsetId }.OrderBy(id => id, StringComparer.Ordinal)) == true
            && sites[2].StateFrame?.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { countId, loadedId, offsetId }.OrderBy(id => id, StringComparer.Ordinal)) == true
            && sites.All(site => site.StateFrame?.TypeId
                == $"type:synthetic:async_state:{site.CallbackId}"),
            "scalar, fixed struct, and object-handle locals should publish exact per-await state frames");
    }

    private static void StructuredFlowProjectsExactStateFrames()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("structured")]
                public static async void Structured()
                {
                    int movementCount;
                    if (ShouldDouble())
                    {
                        movementCount = 2;
                    }
                    else
                    {
                        movementCount = 1;
                    }

                    int overwritten = 4;
                    await AvidContinuations.NextTickAsync();

                    overwritten = 9;
                    int total = movementCount;
                    for (int index = 0; index < 3; ++index)
                    {
                        if (index == 1)
                        {
                            continue;
                        }
                        total += index;
                    }
                    while (total < 8)
                    {
                        ++total;
                        if (total == 7)
                        {
                            break;
                        }
                    }
                    do
                    {
                        --total;
                    }
                    while (total > 6);
                    Consume(overwritten + total);
                }

                private static bool ShouldDouble() => true;
                private static void Consume(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/StructuredAsync.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncAwaitSite awaitSite = method.Segments[0].AwaitSite!;
        string movementCountId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "movementCount").Id;
        string overwrittenId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "overwritten").Id;
        SemanticOperation[] flow = method.Segments
            .SelectMany(segment => segment.Statements)
            .SelectMany(statement => Enumerate(statement.Operation))
            .ToArray();

        Assert(document.Succeeded
            && document.SchemaVersion == 15
            && document.SemanticVersion == "1.17"
            && awaitSite.StateFrame is { } stateFrame
            && stateFrame.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { movementCountId })
            && stateFrame.Slots.All(slot => slot.SymbolId != overwrittenId)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.LocalDeclarationOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.IfOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.ForOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.WhileOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.DoWhileOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.BreakOperationKind)
            && flow.Any(operation => operation.Kind == SemanticAsyncMethod.ContinueOperationKind),
            "structured async flow should preserve only truly live locals across await boundaries");
    }

    private static void NestedAwaitsProjectContinuationCfg()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("nested_cfg")]
                public static async void NestedCfg()
                {
                    int movementCount = 2;
                    for (int index = 0; index < movementCount; ++index)
                    {
                        await AvidContinuations.NextTickAsync();
                        Consume(index);
                    }

                    if (ShouldDelay(movementCount))
                    {
                        await AvidContinuations.DelayAsync(0.1f);
                    }
                    Consume(movementCount);
                }

                private static bool ShouldDelay(int value) => value > 1;
                private static void Consume(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/NestedAwaitCfg.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncSegment[] awaitSegments = method.Segments
            .Where(segment => segment.AwaitSite is not null)
            .ToArray();
        string movementCountId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "movementCount").Id;
        string indexId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "index").Id;

        Assert(document.Succeeded
            && document.SchemaVersion == 15
            && document.SemanticVersion == "1.17"
            && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering
            && method.EntrySegmentOrdinal >= 0
            && method.EntrySegmentOrdinal < method.Segments.Count
            && method.Segments.Select(segment => segment.Ordinal)
                .SequenceEqual(Enumerable.Range(0, method.Segments.Count))
            && method.Segments.All(segment => segment.Transfer is not null)
            && method.Segments.Any(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.BranchTransferKind)
            && method.Segments.Any(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.GotoTransferKind)
            && method.Segments.Any(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.ReturnTransferKind)
            && awaitSegments.Length == 2
            && awaitSegments.All(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.AwaitTransferKind)
            && awaitSegments.Select(segment => segment.AwaitSite!.CallbackId)
                .SequenceEqual(new[] { CompilerCallbackIdStart, CompilerCallbackIdStart + 1 })
            && awaitSegments.All(segment => segment.Transfer!.PrimaryTarget >= 0),
            "nested branch and loop awaits should project one bounded continuation CFG with exact resume targets");
        Assert(document.Reachability!.ReachableCallableIds.Any(id => id.Contains(
                ".ShouldDelay(",
                StringComparison.Ordinal)),
            "continuation CFG branch conditions should retain their synchronous helper call graph");

        SemanticAsyncStateFrame loopFrame = awaitSegments[0].AwaitSite!.StateFrame!;
        SemanticAsyncStateFrame branchFrame = awaitSegments[1].AwaitSite!.StateFrame!;
        Assert(loopFrame.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { indexId, movementCountId }.OrderBy(id => id, StringComparer.Ordinal))
            && branchFrame.Slots.Select(slot => slot.SymbolId)
                .SequenceEqual(new[] { movementCountId }),
            "CFG liveness should preserve loop control state and remove locals that are dead at a later branch await");
    }

    private static void SwitchAwaitsProjectContinuationCfg()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public enum Mode
            {
                Stop,
                Tick,
                Delay,
                AlsoDelay,
            }

            public static class Script
            {
                [AvidExport("switch_cfg")]
                public static async void SwitchCfg()
                {
                    Mode mode = Mode.Delay;
                    switch (mode)
                    {
                        case Mode.Stop:
                            return;
                        case Mode.Tick:
                            await AvidContinuations.NextTickAsync();
                            break;
                        case Mode.Delay:
                        case Mode.AlsoDelay:
                            await AvidContinuations.DelayAsync(0.01f);
                            break;
                        default:
                            await AvidContinuations.NextTickAsync();
                            break;
                    }
                    Consume((int)mode);
                }

                private static void Consume(int value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/SwitchAwaitCfg.cs");
        SemanticAsyncMethod method = document.AsyncMethods.Single();
        SemanticAsyncSegment[] awaitSegments = method.Segments
            .Where(segment => segment.AwaitSite is not null)
            .OrderBy(segment => segment.AwaitSite!.CallbackId)
            .ToArray();
        string modeId = document.Symbols.Single(symbol =>
            symbol.Kind == "local" && symbol.Name == "mode").Id;

        Assert(document.Succeeded
            && method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering
            && awaitSegments.Length == 3
            && method.Segments.Count(segment => segment.Transfer?.Kind
                == SemanticAsyncMethod.BranchTransferKind) == 4
            && awaitSegments.All(segment => segment.AwaitSite!.StateFrame!.Slots.Any(slot =>
                slot.SymbolId == modeId))
            && method.Segments.Where(segment => segment.Transfer?.Kind
                    == SemanticAsyncMethod.BranchTransferKind)
                .All(segment => segment.Transfer!.Condition is
                {
                    Kind: "binary",
                    OperatorKind: "equals",
                    TypeId: "type:bool",
                }),
            "enum switch sections should project deterministic equality dispatch and exact await state frames");

        const string unstableSource = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("unstable_switch")]
                public static async void UnstableSwitch()
                {
                    switch (SelectMode())
                    {
                        case 1:
                            await AvidContinuations.NextTickAsync();
                            break;
                    }
                }

                private static int SelectMode() => 1;
            }
            """;
        SemanticDocument unstable = Analyze(unstableSource, "Scripts/UnstableSwitchAwait.cs");
        Assert(!unstable.Succeeded
            && unstable.AsyncMethods.Count == 0
            && unstable.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5418"),
            "side-effecting switch governing expressions should fail closed until hidden spill locals are frozen");

        const string synchronousSwitchSource = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("synchronous_switch")]
                public static async void SynchronousSwitch()
                {
                    await AvidContinuations.NextTickAsync();
                    int mode = 1;
                    switch (mode)
                    {
                        case 1:
                            Consume(mode);
                            break;
                        default:
                            return;
                    }
                }

                private static void Consume(int value) { }
            }
            """;
        SemanticDocument synchronousSwitch = Analyze(
            synchronousSwitchSource,
            "Scripts/SynchronousSwitchCfg.cs");
        Assert(synchronousSwitch.Succeeded
            && synchronousSwitch.AsyncMethods.Single().Lowering
                == SemanticAsyncMethod.ContinuationCfgLowering,
            "controlled async methods containing synchronous switch dispatch should select continuation CFG lowering");

        const string loopSwitchSource = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("loop_switch")]
                public static async void LoopSwitch()
                {
                    int mode = 0;
                    while (mode < 2)
                    {
                        switch (mode)
                        {
                            case 0:
                                ++mode;
                                await AvidContinuations.NextTickAsync();
                                continue;
                            default:
                                break;
                        }
                        break;
                    }
                    Consume(mode);
                }

                private static void Consume(int value) { }
            }
            """;
        SemanticDocument loopSwitch = Analyze(loopSwitchSource, "Scripts/LoopSwitchCfg.cs");
        Assert(loopSwitch.Succeeded
            && loopSwitch.AsyncMethods.Single().Segments.Single(segment =>
                    segment.AwaitSite is not null)
                .AwaitSite!.StateFrame!.Slots.Any(),
            "switch break should exit the switch while continue retains the enclosing loop target across await");
    }

    private static void NonFrozenAsyncShapesRemainFailClosed()
    {
        const string source = """
            using System.Threading.Tasks;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("task_owner")]
                public static async Task TaskOwner()
                {
                    await Task.Yield();
                }

                [AvidExport("nested")]
                public static async void Nested()
                {
                    Consume(await AvidAssets.LoadObjectAsync("/Game/Valid.Valid"));
                }

                [AvidExport("arbitrary")]
                public static async void ArbitraryAwaiter()
                {
                    await CustomProducer.GetAsync();
                }

                private static void Consume(AvidLoadedObject value) { }
            }

            public static class CustomProducer
            {
                public static AvidVoidAwaitable GetAsync() => default;
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/UnsupportedAsync.cs");

        Assert(!document.Succeeded
            && document.AsyncMethods.Count == 0
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5401")
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5402")
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5403")
            && document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASCS3002") == 3,
            "Task owners, expression-nested awaits, and arbitrary awaiters should retain ASCS3002 and stable async diagnostics");
    }

    private static void CallbackRangesAndAwaitLimitsAreEnforced()
    {
        const string reservedCallbackSource = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(0x40000000)]
                public static void Reserved() { }
            }
            """;
        SemanticDocument reservedCallback = Analyze(
            reservedCallbackSource,
            "Scripts/ReservedCallback.cs");
        Assert(!reservedCallback.Succeeded
            && reservedCallback.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5409"),
            "user continuation ids should remain below the compiler-owned callback range");

        string tooManyAwaits = string.Join(
            Environment.NewLine,
            Enumerable.Repeat("await AvidContinuations.NextTickAsync();", 17));
        string perMethodSource = $$"""
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("too_many")]
                public static async void TooMany()
                {
                    {{tooManyAwaits}}
                }
            }
            """;
        SemanticDocument perMethod = Analyze(perMethodSource, "Scripts/TooManyAwaits.cs");
        Assert(!perMethod.Succeeded
            && perMethod.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5407"),
            "one controlled async method should permit at most sixteen awaits");

        string methods = string.Join(
            Environment.NewLine,
            Enumerable.Range(0, 5).Select(index =>
            {
                string awaits = string.Join(
                    Environment.NewLine,
                    Enumerable.Repeat("await AvidContinuations.NextTickAsync();", 13));
                return $$"""
                    [AvidExport("module_{{index}}")] public static async void Method{{index}}()
                    {
                        {{awaits}}
                    }
                    """;
            }));
        string moduleSource = $$"""
            using AvidScript;
            namespace Game;
            public static class Script
            {
                {{methods}}
            }
            """;
        SemanticDocument module = Analyze(moduleSource, "Scripts/ModuleAwaitLimit.cs");
        Assert(!module.Succeeded
            && module.AsyncMethods.Count == 0
            && module.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5408"),
            "one module should permit at most sixty-four controlled await sites");
    }

    private static void ControlFlowSegmentLimitFailsClosed()
    {
        string trailingStatements = string.Join(
            Environment.NewLine,
            Enumerable.Repeat("Consume();", 63));
        string source = $$"""
            using AvidScript;
            namespace Game;
            public static class Script
            {
                [AvidExport("oversized_cfg")]
                public static async void OversizedControlFlow()
                {
                    while (ShouldContinue())
                    {
                        await AvidContinuations.NextTickAsync();
                    }
                    {{trailingStatements}}
                }

                private static bool ShouldContinue() => false;
                private static void Consume() { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/OversizedAsyncCfg.cs");

        Assert(!document.Succeeded
            && document.AsyncMethods.Count == 0
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5417"),
            "oversized controlled async CFGs should fail closed with ASCS5417 instead of throwing");
    }

    private static SemanticDocument Analyze(string source, string sourceId)
    {
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[]
            {
                new SemanticReferenceSource(
                    AsyncFacade,
                    "generated://AvidScript.Async.generated.cs",
                    true),
            });
    }

    private static System.Collections.Generic.IEnumerable<SemanticOperation> Enumerate(
        SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in Enumerate(child))
            {
                yield return descendant;
            }
        }
    }

    private const string AsyncFacade = """
        using System;
        using System.Runtime.CompilerServices;
        using System.Runtime.InteropServices;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidExportAttribute : Attribute
        {
            public AvidExportAttribute(string exportName) { }
        }

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

        public readonly struct AvidContinuation { }

        public readonly struct AvidVoidAwaitable
        {
            public AvidVoidAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidVoidAwaiter : ICriticalNotifyCompletion
        {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }

        public readonly struct AvidDelayAwaitable
        {
            public AvidDelayAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidDelayAwaiter : ICriticalNotifyCompletion
        {
            public bool IsCompleted => false;
            public void GetResult() { }
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }

        public readonly struct AvidObjectAwaitable
        {
            public AvidObjectAwaiter GetAwaiter() => default;
        }

        public readonly struct AvidObjectAwaiter : ICriticalNotifyCompletion
        {
            public bool IsCompleted => false;
            public AvidLoadedObject GetResult() => default;
            public void OnCompleted(Action continuation) { }
            public void UnsafeOnCompleted(Action continuation) { }
        }

        public readonly struct AvidLoadedObject { }

        public static class AvidContinuations
        {
            public static AvidContinuation Delay(float delaySeconds, int callbackId) => default;
            public static AvidContinuation NextTick(int callbackId) => default;
            public static AvidDelayAwaitable DelayAsync(float delaySeconds) => default;
            public static AvidDelayAwaitable NextTickAsync() => default;
        }

        public static class AvidAssets
        {
            public static AvidContinuation LoadObjectAsync(string assetPath, int callbackId) => default;
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

        internal static class AvidScriptNative
        {
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

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
        LocalsAndObjectResultsCannotCrossTheirBoundaries();
        NonFrozenAsyncShapesRemainFailClosed();
        CallbackRangesAndAwaitLimitsAreEnforced();
        GeneratedLatentProducerProjectsImportIdentity();
        return 5;
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
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/GeneratedLatentAsync.cs");
        SemanticAsyncAwaitSite site = document.AsyncMethods.Single()
            .Segments.Single(segment => segment.AwaitSite is not null)
            .AwaitSite!;
        SemanticCallable import = document.Callables.Single(callable =>
            callable.Import is { Module: "avidscript", Name: "avid_ue_latent_test" });

        Assert(document.Succeeded
            && site.CallbackId == CompilerCallbackIdStart
            && site.ProducerKind == "binding_latent|avidscript|avid_ue_latent_test"
            && site.PayloadKind == "none"
            && site.Arguments.Count == 1
            && site.Arguments[0].TypeId == "type:float32"
            && import.ReturnTypeId == "type:int64"
            && import.Parameters.Select(parameter => parameter.TypeId)
                .SequenceEqual(new[] { "type:float32", "type:int32" }),
            "generated latent markers should project a generic import identity and compiler callback ABI");
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
            && document.SchemaVersion == 12
            && document.SemanticVersion == "1.12"
            && document.AsyncMethods.Count == 2,
            "controlled async exports should publish schema 12 / semantic 1.12");
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

    private static void LocalsAndObjectResultsCannotCrossTheirBoundaries()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidExport("cross_local")]
                public static async void CrossLocal()
                {
                    int count = 1;
                    await AvidContinuations.NextTickAsync();
                    Consume(count);
                }

                [AvidExport("cross_result")]
                public static async void CrossResult()
                {
                    AvidLoadedObject loaded = await AvidAssets.LoadObjectAsync("/Game/Valid.Valid");
                    await AvidContinuations.NextTickAsync();
                    Consume(loaded);
                }

                private static void Consume(int value) { }
                private static void Consume(AvidLoadedObject value) { }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/CrossAwaitLocals.cs");

        Assert(!document.Succeeded
            && document.AsyncMethods.Count == 0
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5405")
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5406")
            && document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASCS3002") == 2,
            "ordinary locals and object results used beyond their permitted segment should fail closed");
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
                    if (true)
                    {
                        await AvidContinuations.NextTickAsync();
                    }
                }

                [AvidExport("arbitrary")]
                public static async void ArbitraryAwaiter()
                {
                    await CustomProducer.GetAsync();
                }
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
            "Task owners, nested control flow, and arbitrary awaiters should retain ASCS3002 and stable async diagnostics");
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
            public static AvidVoidAwaitable DelayAsync(float delaySeconds) => default;
            public static AvidVoidAwaitable NextTickAsync() => default;
        }

        public static class AvidAssets
        {
            public static AvidContinuation LoadObjectAsync(string assetPath, int callbackId) => default;
            public static AvidObjectAwaitable LoadObjectAsync(string assetPath) => default;
        }

        public static class UKismetSystemLibrary
        {
            [AvidLatent("avidscript", "avid_ue_latent_test")]
            public static AvidVoidAwaitable DelayAsync(float Duration) => default;
        }

        internal static class AvidScriptNative
        {
            [DllImport("avidscript", EntryPoint = "avid_ue_latent_test")]
            internal static extern long InvokeLatent(float duration, int callbackId);
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

using System;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticContinuationTests
{
    public static int Run()
    {
        DeterministicCallbacksValidateCallsAndBecomeRoots();
        InvalidHandlersAndCallsFailClosed();
        ObjectLoadPathsFailClosed();
        ProducerPayloadMismatchesFailClosed();
        AsyncAndLambdaRulesRemainFailClosed();
        return 5;
    }

    private static void DeterministicCallbacksValidateCallsAndBecomeRoots()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private const int ResumeSpawn = 1;
                private const string CubePath = "/Engine/EngineMeshes/Cube.Cube";

                [AvidContinuation(2)]
                public static void ResumeSecond() { }

                [AvidContinuation(ResumeSpawn)]
                public static void ResumeSpawnHandler() { }

                [AvidContinuation(3)]
                public static void ResumeObjectLoad(
                    AvidContinuationStatus status,
                    AvidLoadedObject loadedObject) { }

                public static void BeginPlay()
                {
                    AvidContinuations.Delay(0.25f, ResumeSpawn);
                    AvidContinuations.NextTick(2);
                    AvidAssets.LoadObjectAsync(CubePath, 3);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/Continuations.cs");
        SemanticContinuationCallback[] callbacks = document.ContinuationCallbacks.ToArray();

        Assert(document.Succeeded
            && document.SchemaVersion == 11
            && document.SemanticVersion == "1.11",
            "valid continuations should publish semantic schema v11 / version 1.11");
        Assert(callbacks.Select(callback => callback.CallbackId).SequenceEqual(new[] { 1, 2, 3 })
            && callbacks.Select(callback => callback.Name)
                .SequenceEqual(new[] { "ResumeSpawnHandler", "ResumeSecond", "ResumeObjectLoad" })
            && callbacks.Select(callback => callback.PayloadKind)
                .SequenceEqual(new[] { "none", "none", "object" }),
            "continuation callbacks should be ordered deterministically by positive callback id");
        Assert(document.Reachability?.Mode == "entrypoint_roots"
            && callbacks.All(callback => document.Reachability.RootCallableIds.Contains(
                callback.MethodSymbolId,
                StringComparer.Ordinal)),
            "continuation handlers should be compiler entrypoint roots");
        string json = Encoding.UTF8.GetString(SemanticSerializer.Serialize(document));
        int continuationSection = json.IndexOf("\"continuation_callbacks\"", StringComparison.Ordinal);
        int payloadKind = continuationSection >= 0
            ? json.IndexOf("\"payload_kind\"", continuationSection, StringComparison.Ordinal)
            : -1;
        int callbackSpan = payloadKind >= 0
            ? json.IndexOf("\"span\"", payloadKind, StringComparison.Ordinal)
            : -1;
        Assert(json.Contains("\"continuation_callbacks\"", StringComparison.Ordinal)
            && json.IndexOf("\"callback_id\": 1", StringComparison.Ordinal)
                < json.IndexOf("\"callback_id\": 2", StringComparison.Ordinal)
            && json.Contains("\"payload_kind\": \"object\"", StringComparison.Ordinal)
            && continuationSection >= 0
            && payloadKind > continuationSection
            && callbackSpan > payloadKind
            && json.IndexOf("\"delegate_event_callbacks\"", StringComparison.Ordinal)
                < json.IndexOf("\"continuation_callbacks\"", StringComparison.Ordinal)
            && json.IndexOf("\"continuation_callbacks\"", StringComparison.Ordinal)
                < json.IndexOf("\"diagnostics\"", StringComparison.Ordinal),
            "semantic serialization should publish callbacks in callback-id order");
    }

    private static void ObjectLoadPathsFailClosed()
    {
        string boundaryPath = "/Game/" + new string('a', 1016) + ".A";
        string oversizedPath = "/Game/" + new string('\u754c', 339) + ".A";
        string source = $$"""
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private static string DynamicPath = "/Game/Dynamic.Dynamic";

                [AvidContinuation(1)]
                public static void Loaded(
                    AvidContinuationStatus status,
                    AvidLoadedObject loadedObject) { }

                public static void BeginPlay()
                {
                    AvidAssets.LoadObjectAsync(DynamicPath, 1);
                    AvidAssets.LoadObjectAsync("", 1);
                    AvidAssets.LoadObjectAsync("Game/Relative.Relative", 1);
                    AvidAssets.LoadObjectAsync("/Game/Bad\0.Bad", 1);
                    AvidAssets.LoadObjectAsync("{{boundaryPath}}", 1);
                    AvidAssets.LoadObjectAsync("{{oversizedPath}}", 1);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/InvalidObjectLoadPaths.cs");
        string[] diagnostics = document.Diagnostics
            .Select(diagnostic => $"{diagnostic.Code}:{diagnostic.Message}")
            .ToArray();

        Assert(!document.Succeeded
            && diagnostics.SequenceEqual(new[]
            {
                "ASCS5306:AvidAssets.LoadObjectAsync requires a compile-time nonempty asset path.",
                "ASCS5306:AvidAssets.LoadObjectAsync requires a compile-time nonempty asset path.",
                "ASCS5307:AvidAssets.LoadObjectAsync asset path must be canonical-looking and contain no NUL character.",
                "ASCS5307:AvidAssets.LoadObjectAsync asset path must be canonical-looking and contain no NUL character.",
                "ASCS5308:AvidAssets.LoadObjectAsync asset path must not exceed 1024 UTF-8 bytes.",
            }),
            "object loads should reject dynamic, empty, noncanonical, NUL, and oversized paths deterministically");
    }

    private static void ProducerPayloadMismatchesFailClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(1)]
                public static void Loaded(
                    AvidContinuationStatus status,
                    AvidLoadedObject loadedObject) { }

                [AvidContinuation(2)]
                public static void TimerElapsed() { }

                public static void BeginPlay()
                {
                    AvidAssets.LoadObjectAsync("/Game/Valid.Valid", 1);
                    AvidContinuations.NextTick(1);
                    AvidContinuations.Delay(0.25f, 2);
                    AvidAssets.LoadObjectAsync("/Game/Valid.Valid", 2);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/ContinuationProducerMismatch.cs");
        string[] diagnostics = document.Diagnostics
            .Select(diagnostic => $"{diagnostic.Code}:{diagnostic.Message}")
            .ToArray();

        Assert(!document.Succeeded
            && diagnostics.SequenceEqual(new[]
            {
                "ASCS5310:AvidContinuations.NextTick callback id '1' requires a zero-parameter continuation handler.",
                "ASCS5309:AvidAssets.LoadObjectAsync callback id '2' requires an object-payload continuation handler.",
            }),
            "callback ids reused across incompatible producer payload shapes should fail closed");
    }

    private static void InvalidHandlersAndCallsFailClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private static int DynamicId = 8;

                [AvidContinuation(0)] public static void Zero() { }
                [AvidContinuation(3)] private static void Hidden() { }
                [AvidContinuation(4)] public static void Generic<T>() { }
                [AvidContinuation(5)] public static int ReturnsValue() => 0;
                [AvidContinuation(6)] public static void HasParameter(int value) { }
                [AvidContinuation(7)] public static void First() { }
                [AvidContinuation(7)] public static void Duplicate() { }
                [AvidContinuation(8)] public static void SwappedPayload(
                    AvidLoadedObject loadedObject,
                    AvidContinuationStatus status) { }
                [AvidContinuation(9)] public static void RefPayload(
                    ref AvidContinuationStatus status,
                    AvidLoadedObject loadedObject) { }

                public static void BeginPlay()
                {
                    AvidContinuations.NextTick(DynamicId);
                    AvidContinuations.NextTick(99);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/InvalidContinuations.cs");
        string[] codes = document.Diagnostics.Select(diagnostic => diagnostic.Code).ToArray();

        Assert(!document.Succeeded
            && codes.Contains("ASCS5301")
            && codes.Contains("ASCS5302")
            && codes.Contains("ASCS5303")
            && codes.Contains("ASCS5304")
            && codes.Contains("ASCS5305")
            && document.Diagnostics.Count(diagnostic => diagnostic.Code == "ASCS5303") >= 6,
            "invalid ids, handlers, and continuation call sites should fail closed");
        Assert(document.ContinuationCallbacks.Count == 0
            && document.ControlFlowGraphs.Count == 0,
            "continuation errors should not publish partial callbacks or executable graphs");
    }

    private static void AsyncAndLambdaRulesRemainFailClosed()
    {
        const string source = """
            using System;
            using System.Threading.Tasks;
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidContinuation(1)]
                public static void WithLambda()
                {
                    Action callback = () => { };
                    callback();
                }

                [AvidContinuation(2)]
                public static async Task AsyncHandler()
                {
                    await Task.Yield();
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/UnsupportedContinuationFlow.cs");

        Assert(!document.Succeeded
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS4001")
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS3002"),
            "continuation handlers must not enable lambda, closure, or async lowering");
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
                    ContinuationFacade,
                    "generated://AvidScript.Continuations.generated.cs",
                    true),
            });
    }

    private const string ContinuationFacade = """
        using System;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class AvidContinuationAttribute : Attribute
        {
            public AvidContinuationAttribute(int callbackId) { }
        }

        public readonly struct AvidContinuation
        {
            private readonly long Token;
            internal AvidContinuation(long token) { Token = token; }
        }

        public static class AvidContinuations
        {
            public static AvidContinuation Delay(float delaySeconds, int callbackId) => new(0);
            public static AvidContinuation NextTick(int callbackId) => new(0);
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
            public static AvidContinuation LoadObjectAsync(string assetPath, int callbackId) => new(0);
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

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
        AsyncAndLambdaRulesRemainFailClosed();
        return 3;
    }

    private static void DeterministicCallbacksValidateCallsAndBecomeRoots()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                private const int ResumeSpawn = 1;

                [AvidContinuation(2)]
                public static void ResumeSecond() { }

                [AvidContinuation(ResumeSpawn)]
                public static void ResumeSpawnHandler() { }

                public static void BeginPlay()
                {
                    AvidContinuations.Delay(0.25f, ResumeSpawn);
                    AvidContinuations.NextTick(2);
                }
            }
            """;

        SemanticDocument document = Analyze(source, "Scripts/Continuations.cs");
        SemanticContinuationCallback[] callbacks = document.ContinuationCallbacks.ToArray();

        Assert(document.Succeeded
            && document.SchemaVersion == 10
            && document.SemanticVersion == "1.10",
            "valid continuations should publish semantic schema v10 / version 1.10");
        Assert(callbacks.Select(callback => callback.CallbackId).SequenceEqual(new[] { 1, 2 })
            && callbacks.Select(callback => callback.Name)
                .SequenceEqual(new[] { "ResumeSpawnHandler", "ResumeSecond" }),
            "continuation callbacks should be ordered deterministically by positive callback id");
        Assert(document.Reachability?.Mode == "entrypoint_roots"
            && callbacks.All(callback => document.Reachability.RootCallableIds.Contains(
                callback.MethodSymbolId,
                StringComparer.Ordinal)),
            "continuation handlers should be compiler entrypoint roots");
        string json = Encoding.UTF8.GetString(SemanticSerializer.Serialize(document));
        Assert(json.Contains("\"continuation_callbacks\"", StringComparison.Ordinal)
            && json.IndexOf("\"callback_id\": 1", StringComparison.Ordinal)
                < json.IndexOf("\"callback_id\": 2", StringComparison.Ordinal)
            && json.IndexOf("\"delegate_event_callbacks\"", StringComparison.Ordinal)
                < json.IndexOf("\"continuation_callbacks\"", StringComparison.Ordinal)
            && json.IndexOf("\"continuation_callbacks\"", StringComparison.Ordinal)
                < json.IndexOf("\"diagnostics\"", StringComparison.Ordinal),
            "semantic serialization should publish callbacks in callback-id order");
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
            && codes.Contains("ASCS5305"),
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
        """;

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

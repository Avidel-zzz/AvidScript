using System;
using System.Linq;
using System.Text;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticDelegateEventTests
{
    private const string SignalId =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    private const string OtherId =
        "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    private const string UnknownId =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    public static int Run()
    {
        GeneratedContractsProjectTypedHandlers();
        InvalidHandlersFailClosed();
        MalformedAndDuplicateContractsFailClosed();
        DelegateSyntaxRemainsUnsupported();
        return 4;
    }

    private static void GeneratedContractsProjectTypedHandlers()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal(AActor actor, Payload payload, SignalKind kind) { }
            }
            """;
        SemanticDocument document = Analyze(source, Contracts(
            $"[AvidEventContract(\"{SignalId}\", \"global::AvidScript.AActor;global::AvidScript.Payload;global::AvidScript.SignalKind\")]\n" +
            $"public const string OnSignal = \"{SignalId}\";"));

        SemanticDelegateEventCallback callback = document.DelegateEventCallbacks.Single();
        Assert(document.Succeeded
            && document.SchemaVersion == 13
            && document.SemanticVersion == "1.13",
            "valid delegate event contracts should publish semantic schema v13");
        Assert(callback.SubscriptionId == SignalId
            && callback.ExportName == "avid_on_delegate_0123456789abcdef"
            && callback.Name == "HandleSignal",
            "delegate event callback should retain stable subscription and export identities");
        Assert(document.Reachability?.Mode == "entrypoint_roots"
            && document.Reachability.RootCallableIds.Contains(callback.MethodSymbolId),
            "delegate event handlers should be compiler entrypoint roots");
        string json = Encoding.UTF8.GetString(SemanticSerializer.Serialize(document));
        Assert(json.Contains("\"delegate_event_callbacks\"", StringComparison.Ordinal),
            "semantic serialization should publish delegate event callback metadata");
    }

    private static void InvalidHandlersFailClosed()
    {
        string source = $$"""
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent("{{UnknownId}}")] public static void Unknown(AActor actor) { }
                [AvidEvent(AvidEvents.OnSignal)] private static void Hidden(AActor actor, Payload payload, SignalKind kind) { }
                [AvidEvent(AvidEvents.OnSignal)] public static int Duplicate(AActor actor, Payload payload, SignalKind kind) => 0;
                [AvidEvent(AvidEvents.OnOther)] public static void Wrong(AActor actor, Payload payload) { }
                [AvidEvent(AvidEvents.OnOther)] public static void Generic<T>(AActor actor, Payload payload, SignalKind kind) { }
            }
            """;
        SemanticDocument document = Analyze(source, Contracts(
            $"[AvidEventContract(\"{SignalId}\", \"global::AvidScript.AActor;global::AvidScript.Payload;global::AvidScript.SignalKind\")]\n" +
            $"public const string OnSignal = \"{SignalId}\";\n" +
            $"[AvidEventContract(\"{OtherId}\", \"global::AvidScript.AActor;global::AvidScript.Payload;global::AvidScript.SignalKind\")]\n" +
            $"public const string OnOther = \"{OtherId}\";"));

        string[] codes = document.Diagnostics.Select(diagnostic => diagnostic.Code).ToArray();
        Assert(!document.Succeeded
            && codes.Contains("ASCS5203")
            && codes.Contains("ASCS5204")
            && codes.Contains("ASCS5205")
            && codes.Contains("ASCS5206")
            && codes.Contains("ASCS5207"),
            "unknown, invalid, mismatched, and duplicate handlers should fail closed");
        Assert(document.DelegateEventCallbacks.Count == 0
            && document.ControlFlowGraphs.Count == 0,
            "delegate event errors should not publish partial callbacks or executable graphs");
    }

    private static void MalformedAndDuplicateContractsFailClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.First)]
                public static void Handle(AActor actor) { }
            }
            """;
        string members =
            $"[AvidEventContract(\"{SignalId}\", \"global::AvidScript.AActor\")]\n" +
            $"public const string First = \"{SignalId}\";\n" +
            $"[AvidEventContract(\"{SignalId}\", \"global::AvidScript.AActor\")]\n" +
            $"public const string Duplicate = \"{SignalId}\";\n" +
            $"[AvidEventContract(\"not-a-hash\", \"global::AvidScript.AActor\")]\n" +
            "public const string Broken = \"not-a-hash\";";
        SemanticDocument document = Analyze(source, Contracts(members));

        Assert(!document.Succeeded
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5201")
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5202")
            && document.DelegateEventCallbacks.Count == 0,
            "malformed and duplicate generated contracts should fail closed");
    }

    private static void DelegateSyntaxRemainsUnsupported()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal(AActor actor, Payload payload, SignalKind kind)
                {
                    System.Action action = () => { };
                    action();
                }
            }
            """;
        SemanticDocument document = Analyze(source, Contracts(
            $"[AvidEventContract(\"{SignalId}\", \"global::AvidScript.AActor;global::AvidScript.Payload;global::AvidScript.SignalKind\")]\n" +
            $"public const string OnSignal = \"{SignalId}\";"));

        Assert(!document.Succeeded
            && document.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS4001"),
            "AvidEvent support must not enable delegates, lambdas, or closures");
    }

    private static SemanticDocument Analyze(string source, string generatedSource)
    {
        const string sourceId = "Scripts/DelegateEvents.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[]
            {
                new SemanticReferenceSource(
                    generatedSource,
                    "generated://AvidScript.DelegateEvents.generated.cs",
                    true),
            });
    }

    private static string Contracts(string members)
    {
        return $$"""
            using System;

            namespace AvidScript;

            [AttributeUsage(AttributeTargets.Method)]
            public sealed class AvidEventAttribute : Attribute
            {
                public AvidEventAttribute(string subscriptionId) { }
            }

            [AttributeUsage(AttributeTargets.Field)]
            public sealed class AvidEventContractAttribute : Attribute
            {
                public AvidEventContractAttribute(string subscriptionId, string parameterTypes) { }
            }

            public readonly struct AActor
            {
                public readonly int Slot;
                public readonly int Generation;
            }

            public readonly struct Payload
            {
                public readonly int Count;
            }

            public enum SignalKind { First = 1 }

            public static class AvidEvents
            {
            {{members}}
            }
            """;
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

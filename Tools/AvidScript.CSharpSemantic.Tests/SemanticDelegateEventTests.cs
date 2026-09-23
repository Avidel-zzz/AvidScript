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
        RefOutContractsProjectTransactionalHandlers();
        ReturnContractsProjectTypedHandlers();
        InvalidHandlersFailClosed();
        MalformedAndDuplicateContractsFailClosed();
        NoncapturingLambdaInEventHandlerIsSupported();
        GeneratedLanguageEventAssignmentsProjectOnce();
        InvalidLanguageEventAssignmentsFailClosed();
        return 8;
    }

    private static void GeneratedLanguageEventAssignmentsProjectOnce()
    {
        const string source = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                static void Handle(float value) { }
                public static void Run()
                {
                    AActor actor = default;
                    Handler handler = Handle;
                    actor.Signal += handler;
                    actor.Signal -= handler;
                }
            }
            """;
        SemanticDocument document = Analyze(source, LanguageFacade());
        Assert(document.Succeeded, "generated language events should analyze: " +
            string.Join(" | ", document.Diagnostics.Select(diagnostic => diagnostic.Message)));
        SemanticEventSubscription entry = document.EventSubscriptions.Single();
        SemanticOperation[] assignments = document.Methods.SelectMany(method => Flatten(method.Root))
            .Where(operation => operation.Kind == "event_assignment").ToArray();
        Assert(entry.EventSymbolId is not null && entry.OwnerTypeId == "type:global::AvidScript.AActor"
            && assignments.Length == 2
            && assignments.Select(operation => operation.OperatorKind).SequenceEqual(new[] { "add", "remove" })
            && assignments.All(operation => operation.SymbolId == entry.EventSymbolId
                && operation.Children.Count == 2
                && operation.Children[0].Kind == "event_reference"
                && operation.Children[1].TypeId == entry.DelegateTypeId)
            && SemanticEventSubscriptionValidator.IsValid(document),
            "event assignments should preserve exact owner, event, and handler identities");
        SemanticDocument decoded = SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document));
        Assert(SemanticEventSubscriptionValidator.IsValid(decoded)
            && decoded.EventSubscriptions.Single().EventSymbolId == entry.EventSymbolId,
            "language event metadata should survive canonical serialization");
    }

    private static void InvalidLanguageEventAssignmentsFailClosed()
    {
        const string source = """
            using AvidScript;
            namespace Game;
            public static class Script
            {
                static void Handle(float value) { }
                public static void Run()
                {
                    AActor actor = default;
                    actor.Signal += Handle;
                }
            }
            """;
        SemanticDocument document = Analyze(source, LanguageFacade());
        Assert(document.Succeeded && SemanticEventSubscriptionValidator.IsValid(document),
            "valid language event assignment should be accepted");
        SemanticEventSubscription entry = document.EventSubscriptions.Single();
        Assert(!SemanticEventSubscriptionValidator.IsValid(document with
            { EventSubscriptions = new[] { entry with { EventSymbolId = "symbol:event:forged" } } })
            && !SemanticEventSubscriptionValidator.IsValid(document with
            { EventSubscriptions = new[] { entry with { OwnerTypeId = "type:global::Game.Script" } } })
            && !SemanticEventSubscriptionValidator.IsValid(document with
            { SchemaVersion = 29, SemanticVersion = "1.33" })
            && !SemanticEventSubscriptionValidator.IsValid(document with
            {
                SchemaVersion = 29, SemanticVersion = "1.33",
                EventSubscriptions = new[] { entry with { EventSymbolId = null, OwnerTypeId = null } },
            })
            && !SemanticEventSubscriptionValidator.IsValid(document with
            {
                SchemaVersion = 28, SemanticVersion = "1.32",
                EventSubscriptions = Array.Empty<SemanticEventSubscription>(),
            }), "forged metadata and legacy event assignments must fail closed");
        SemanticDocument badOrdinal = Analyze(source, LanguageFacade().Replace(
            "[AvidEventLanguage(Events.Signal, 7)]", "[AvidEventLanguage(Events.Signal, 8)]"));
        Assert(!badOrdinal.Succeeded && badOrdinal.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5210"),
            "mismatched generated event ordinal must be diagnosed");
        string userEventSource = source.Replace("namespace Game;", "")
            .Replace("actor.Signal += Handle;", "new Custom().Signal += Handle;")
            + "\npublic sealed class Custom { public event AvidScript.Handler Signal { add { } remove { } } }";
        SemanticDocument userEvent = Analyze(userEventSource, LanguageFacade());
        Assert(!userEvent.Succeeded && userEvent.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCS5211"),
            "non-generated C# events must not silently lower as UE events");
    }

    private static string LanguageFacade() => $$"""
        using System;
        using System.Runtime.CompilerServices;
        namespace AvidScript;
        [AttributeUsage(AttributeTargets.Field)] internal sealed class AvidEventContractAttribute : Attribute
        {
            public AvidEventContractAttribute(string id, string parameters, string directions, string result) { }
        }
        [AttributeUsage(AttributeTargets.Method)] internal sealed class AvidEventSubscriptionAttribute : Attribute
        {
            public AvidEventSubscriptionAttribute(string id, int ordinal) { }
        }
        [AttributeUsage(AttributeTargets.Event)] internal sealed class AvidEventLanguageAttribute : Attribute
        {
            public AvidEventLanguageAttribute(string id, int ordinal) { }
        }
        public delegate void Handler(float value);
        public readonly struct AActor
        {
            public readonly int Slot;
            public readonly int Generation;
            [AvidEventLanguage(Events.Signal, 7)]
            public event Handler Signal { add { } remove { } }
        }
        public static class Events
        {
            [AvidEventContract("{{SignalId}}", "global::System.Single", "none", "global::System.Void")]
            public const string Signal = "{{SignalId}}";
            [AvidEventSubscription(Signal, 7)]
            [MethodImpl(MethodImplOptions.InternalCall)]
            public static extern long Subscribe(int slot, int generation, Handler handler);
        }
        """;

    private static System.Collections.Generic.IEnumerable<SemanticOperation> Flatten(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation nested in Flatten(child)) yield return nested;
    }

    private static void ReturnContractsProjectTypedHandlers()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static int HandleSignal(ref int value, out int doubled)
                {
                    value += 3;
                    doubled = value * 2;
                    return doubled + 1;
                }
            }
            """;
        SemanticDocument document = Analyze(source, Contracts(
            $"[AvidEventContract(\"{SignalId}\", \"global::System.Int32;global::System.Int32\", \"ref;out\", \"global::System.Int32\")]\n" +
            $"public const string OnSignal = \"{SignalId}\";"));

        SemanticCallable callable = document.Callables.Single(candidate =>
            candidate.MethodSymbolId == document.DelegateEventCallbacks.Single().MethodSymbolId);
        Assert(document.Succeeded
            && callable.ReturnTypeId == "type:int32"
            && callable.Parameters.Select(parameter => parameter.RefKind)
                .SequenceEqual(new[] { "ref", "out" }),
            "generated return contracts should authorize an exact non-void ref/out handler");
    }

    private static void RefOutContractsProjectTransactionalHandlers()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal(ref int value, out int doubled)
                {
                    value += 3;
                    doubled = value * 2;
                }
            }
            """;
        SemanticDocument document = Analyze(source, Contracts(
            $"[AvidEventContract(\"{SignalId}\", \"global::System.Int32;global::System.Int32\", \"ref;out\")]\n" +
            $"public const string OnSignal = \"{SignalId}\";"));

        Assert(document.Succeeded
            && document.DelegateEventCallbacks.Count == 1
            && document.Callables.Single(callable =>
                    callable.MethodSymbolId == document.DelegateEventCallbacks[0].MethodSymbolId)
                .Parameters.Select(parameter => parameter.RefKind)
                .SequenceEqual(new[] { "ref", "out" }),
            "generated direction contracts should authorize exact ref/out handler parameters");
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

                [AvidEvent(AvidEvents.OnScalar)]
                public static void HandleScalar(int value) { }
            }
            """;
        SemanticDocument document = Analyze(source, Contracts(
            $"[AvidEventContract(\"{SignalId}\", \"global::AvidScript.AActor;global::AvidScript.Payload;global::AvidScript.SignalKind\")]\n" +
            $"public const string OnSignal = \"{SignalId}\";\n" +
            $"[AvidEventContract(\"{OtherId}\", \"global::System.Int32\")]\n" +
            $"public const string OnScalar = \"{OtherId}\";"));

        SemanticDelegateEventCallback callback = document.DelegateEventCallbacks.Single(
            candidate => candidate.SubscriptionId == SignalId);
        Assert(document.Succeeded
            && document.SchemaVersion == 30
            && document.SemanticVersion == "1.36"
            && document.DelegateEventCallbacks.Count == 2,
            "valid delegate event contracts should publish semantic schema v30");
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

    private static void NoncapturingLambdaInEventHandlerIsSupported()
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

        Assert(document.Succeeded
            && document.Callables.Any(callable => callable.MethodSymbolId.Contains(":lambda:", StringComparison.Ordinal)),
            "AvidEvent handlers may use synchronous noncapturing lambdas");
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
                public AvidEventContractAttribute(
                    string subscriptionId,
                    string parameterTypes,
                    string parameterDirections) { }
                public AvidEventContractAttribute(
                    string subscriptionId,
                    string parameterTypes,
                    string parameterDirections,
                    string returnType) { }
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

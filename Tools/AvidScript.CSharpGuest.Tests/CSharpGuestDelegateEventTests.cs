using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

internal static class CSharpGuestDelegateEventTests
{
    private const string SemanticHash =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    private const string SignalId =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    public static int Run()
    {
        FixedLayoutsProduceTypedExports();
        AbiCellLimitFailsClosed();
        UnsupportedNestedTypesFailClosed();
        TamperedCallbackMetadataFailsClosed();
        return 4;
    }

    private static void FixedLayoutsProduceTypedExports()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal(AActor actor, EventPayload payload, SignalKind kind) { }
            }
            """;
        SemanticDocument document = Analyze(source, GeneratedFacade(
            "global::AvidScript.AActor;global::AvidScript.EventPayload;global::AvidScript.SignalKind",
            """
            public readonly struct AActor
            {
                public readonly int Slot;
                public readonly int Generation;
            }

            public readonly struct FVector
            {
                public readonly float X;
                public readonly float Y;
                public readonly float Z;
            }

            public readonly struct EventPayload
            {
                public readonly FVector Position;
                public readonly int Count;
            }

            public enum SignalKind { First = 1 }
            """));

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);
        GuestModule module = result.Module
            ?? throw new InvalidOperationException("typed delegate event produced no Guest module");
        GuestExport export = module.Exports.Single(item =>
            item.Name == "avid_on_delegate_0123456789abcdef");
        GuestFunction wrapper = module.Functions.Single(function => function.Id == export.FunctionId);
        GuestInstruction[] instructions = wrapper.Blocks.SelectMany(block => block.Instructions).ToArray();

        Assert(result.Succeeded
            && wrapper.Parameters.Select(parameter => module.Types.Single(type => type.Id == parameter.TypeId).Storage)
                .SequenceEqual(new[] { "i32", "i32", "f32", "f32", "f32", "i32", "i32" }),
            "fixed layouts should recursively flatten into one typed seven-cell export ABI");
        Assert(instructions.Count(instruction => instruction.Op == "stack_alloc") == 3
            && instructions.Count(instruction => instruction.Op == "field_store") == 7,
            "delegate wrapper should rebuild nested FVector, payload, and object facade values");
        Assert(instructions.Single(instruction => instruction.Op == "call").TargetId ==
            "function:" + document.DelegateEventCallbacks.Single().MethodSymbolId,
            "delegate wrapper should invoke the projected C# handler");
        Assert(GuestModuleValidator.Validate(module).Succeeded,
            "typed delegate event wrapper should pass independent Guest IR validation");
    }

    private static void AbiCellLimitFailsClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal(WidePayload payload) { }
            }
            """;
        SemanticDocument document = Analyze(source, GeneratedFacade(
            "global::AvidScript.WidePayload",
            """
            public readonly struct WidePayload
            {
                public readonly long A;
                public readonly long B;
                public readonly long C;
                public readonly long D;
                public readonly long E;
            }
            """));

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1008"
                && diagnostic.Message.Contains("requires 10 ABI cells", StringComparison.Ordinal)),
            "delegate event ABIs larger than eight 32-bit cells should fail closed");
    }

    private static void UnsupportedNestedTypesFailClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal(TextPayload payload) { }
            }
            """;
        SemanticDocument document = Analyze(source, GeneratedFacade(
            "global::AvidScript.TextPayload",
            """
            public readonly struct TextPayload
            {
                public readonly string Value;
            }
            """));

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, SemanticHash);

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1008"
                && diagnostic.Message.Contains("not a scalar, enum, or fixed-layout struct", StringComparison.Ordinal)),
            "reference-backed or variable-layout event payloads should fail closed");
    }

    private static void TamperedCallbackMetadataFailsClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            public static class Script
            {
                [AvidEvent(AvidEvents.OnSignal)]
                public static void HandleSignal(SignalKind kind) { }
            }
            """;
        SemanticDocument document = Analyze(source, GeneratedFacade(
            "global::AvidScript.SignalKind",
            "public enum SignalKind { First = 1 }"));
        SemanticDelegateEventCallback callback = document.DelegateEventCallbacks.Single();
        SemanticDocument tampered = document with
        {
            DelegateEventCallbacks = new[] { callback with { ExportName = "avid_on_delegate_tampered" } },
        };

        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(tampered, SemanticHash);

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "guest input validation should reject tampered delegate callback identities");
    }

    private static SemanticDocument Analyze(string source, string generatedSource)
    {
        const string sourceId = "Scripts/DelegateEventGuest.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument document = SemanticAnalyzer.Analyze(
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
        Assert(document.Succeeded, "delegate event source should produce a valid semantic artifact");
        return document;
    }

    private static string GeneratedFacade(string parameterTypes, string typeDeclarations)
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

            {{typeDeclarations}}

            public static class AvidEvents
            {
                [AvidEventContract("{{SignalId}}", "{{parameterTypes}}")]
                public const string OnSignal = "{{SignalId}}";
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

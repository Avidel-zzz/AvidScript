using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;

internal static class CSharpGuestMalformedTests
{
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        MalformedSemanticObjectFailsClosed();
        FutureAndMismatchedSemanticVersionsFailClosed();
        GameplayReachabilityModeMismatchFailsClosed();
        GameplayCallbackDescriptorMismatchFailsClosed();
        DuplicateGameplayPayloadFieldsFailClosedWithoutThrowing();
        return 5;
    }

    private static void FutureAndMismatchedSemanticVersionsFailClosed()
    {
        SemanticDocument baseline = CSharpGuestSemanticFixture.Create();
        SemanticDocument future = baseline with { SchemaVersion = 17, SemanticVersion = "1.18" };
        SemanticDocument mismatched = baseline with { SchemaVersion = 10, SemanticVersion = "1.9" };

        AssertRejected(future, "future semantic schemas should be rejected");
        AssertRejected(mismatched, "schema and semantic versions should use a supported pair");
    }

    private static void GameplayReachabilityModeMismatchFailsClosed()
    {
        SemanticDocument baseline = AnalyzeGameplaySource();
        SemanticDocument exportMode = baseline with
        {
            Reachability = baseline.Reachability! with { Mode = "export_roots" },
        };
        SemanticDocument compatibilityMode = baseline with
        {
            Reachability = new SemanticReachability(
                "all_callables_compatibility",
                Array.Empty<string>(),
                baseline.Callables.Select(callable => callable.MethodSymbolId)
                    .OrderBy(id => id, StringComparer.Ordinal)
                    .ToArray(),
                Array.Empty<SemanticReachableImport>()),
        };

        AssertRejected(exportMode, "callbacks should require entrypoint_roots mode");
        AssertRejected(compatibilityMode, "schema 7 callbacks should reject compatibility reachability mode");
    }

    private static void GameplayCallbackDescriptorMismatchFailsClosed()
    {
        SemanticDocument baseline = AnalyzeGameplaySource();
        SemanticGameplayEventCallback callback = baseline.GameplayEventCallbacks.Single();
        SemanticCallable helper = baseline.Callables.Single(callable =>
            callable.MethodSymbolId.Contains(".Helper(", StringComparison.Ordinal));
        string[] roots = baseline.Callables
            .Where(callable => callable.Export is not null)
            .Select(callable => callable.MethodSymbolId)
            .Append(helper.MethodSymbolId)
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToArray();
        SemanticDocument forged = baseline with
        {
            GameplayEventCallbacks = new[] { callback with { MethodSymbolId = helper.MethodSymbolId } },
            Reachability = new SemanticReachability(
                "entrypoint_roots",
                roots,
                roots,
                Array.Empty<SemanticReachableImport>()),
        };

        AssertRejected(forged, "callback descriptors should remain bound to their named method contract");
    }

    private static void DuplicateGameplayPayloadFieldsFailClosedWithoutThrowing()
    {
        SemanticDocument baseline = AnalyzeGameplaySource();
        SemanticSymbol actionId = baseline.Symbols.Single(symbol =>
            symbol.Kind == "field"
            && symbol.Name == "ActionId"
            && symbol.ContainingSymbolId == "symbol:type:global::AvidScript.InputEvent");
        SemanticSymbol duplicate = actionId with
        {
            Id = "symbol:field:global::AvidScript.InputEvent.ActionId:duplicate",
            Signature = "ActionId:duplicate",
        };
        SemanticDocument malformed = baseline with
        {
            Symbols = baseline.Symbols.Append(duplicate).ToArray(),
        };

        AssertRejected(malformed, "duplicate gameplay payload field names should fail closed");
    }

    private static void MalformedSemanticObjectFailsClosed()
    {
        SemanticDocument malformed = CSharpGuestSemanticFixture.Create() with { Types = null! };
        CSharpGuestLoweringResult nullResult = CSharpGuestLowerer.Lower(malformed, SemanticHash);
        SemanticDocument duplicate = CSharpGuestSemanticFixture.Create();
        duplicate = duplicate with { Types = duplicate.Types.Append(duplicate.Types[0]).ToArray() };
        CSharpGuestLoweringResult duplicateResult = CSharpGuestLowerer.Lower(duplicate, SemanticHash);

        Assert(!nullResult.Succeeded && nullResult.Module is null
            && nullResult.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "null semantic collections should return ASCG1001 instead of throwing");
        Assert(!duplicateResult.Succeeded && duplicateResult.Module is null
            && duplicateResult.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            "duplicate semantic identities should return ASCG1001 instead of throwing");
    }

    private static SemanticDocument AnalyzeGameplaySource()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct FVector
            {
                public readonly float X;
                public readonly float Y;
                public readonly float Z;
            }

            public readonly struct InputEvent
            {
                public readonly int ActionId;
                public readonly int TriggerEvent;
                public readonly FVector Value;
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds) { }

                public static void OnInput(InputEvent input) { }
                public static void Helper(InputEvent input) { }
            }
            """;
        const string sourceId = "Scripts/MalformedGameplayArtifact.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
        Assert(semantic.Succeeded, "malformed gameplay fixture baseline should analyze successfully");
        return semantic;
    }

    private static void AssertRejected(SemanticDocument document, string message)
    {
        CSharpGuestLoweringResult result;
        try
        {
            result = CSharpGuestLowerer.Lower(document, SemanticHash);
        }
        catch (Exception exception)
        {
            throw new InvalidOperationException(message + " without throwing", exception);
        }

        Assert(!result.Succeeded
            && result.Module is null
            && result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASCG1001"),
            message);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

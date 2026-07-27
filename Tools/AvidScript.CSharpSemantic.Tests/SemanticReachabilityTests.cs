using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticReachabilityTests
{
    public static int Run()
    {
        ExportRootsRetainOnlyReachableFacadeAndPropertyImports();
        PropertyWriteRetainsOnlySetterAccessor();
        PropertyReadRetainsOnlyGetterAccessor();
        PropertyReadModifyWriteRetainsBothAccessors();
        InputsWithoutExportsPreserveAllCallablesForAnalysis();
        return 5;
    }

    private static void ExportRootsRetainOnlyReachableFacadeAndPropertyImports()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void BeginPlay()
                {
                    Api.Used();
                    _ = UE.Self;
                }
            }
            """;
        const string generatedSource = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Api
            {
                public static void Used() => Native.Used();
                public static void Unused() => Native.Unused();
            }

            public static class UE
            {
                public static int Self => Native.Owner();
            }

            internal static class Native
            {
                [DllImport("avidscript", EntryPoint = "avid_ue_used")]
                internal static extern void Used();

                [DllImport("avidscript", EntryPoint = "avid_ue_unused")]
                internal static extern void Unused();

                [DllImport("env", EntryPoint = "owner_get_slot")]
                internal static extern int Owner();
            }
            """;
        const string sourceId = "Scripts/Reachability.cs";
        string sourceHash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;

        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            sourceHash,
            new[]
            {
                new SemanticReferenceSource(
                    generatedSource,
                    "generated://AvidScript.Bindings.generated.cs",
                    IsExecutable: true),
            });

        Assert(document.Succeeded, "reachable generated facade source should analyze");
        Assert(document.SchemaVersion == 8 && document.SemanticVersion == "1.8",
            "reachability requires the explicit semantic schema version");
        SemanticReachability reachability = document.Reachability
            ?? throw new InvalidOperationException("schema 6 semantic output omitted reachability");
        Assert(reachability.Mode == "export_roots"
            && reachability.RootCallableIds.Count == 1,
            "lifecycle exports should be the only reachability roots");
        string[] importNames = reachability.ReachableImports
            .Select(import => import.Name)
            .ToArray();
        Assert(importNames.SequenceEqual(new[] { "avid_ue_used", "owner_get_slot" }),
            "reachability should retain the used facade and property getter imports deterministically");
        Assert(!reachability.ReachableCallableIds.Any(id =>
                id.Contains(".Unused(", StringComparison.Ordinal))
            && reachability.ReachableImports.All(import => import.Name != "avid_ue_unused"),
            "unused generated facade methods and imports must stay outside the closure");
    }

    private static void InputsWithoutExportsPreserveAllCallablesForAnalysis()
    {
        const string source = """
            using System.Runtime.InteropServices;

            static class Library
            {
                public static void Helper() { }

                [DllImport("env", EntryPoint = "host_probe")]
                internal static extern void Probe();
            }
            """;
        const string sourceId = "Scripts/LibraryAnalysis.cs";
        string sourceHash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;

        SemanticDocument document = SemanticAnalyzer.Analyze(source, sourceId, sourceHash);

        Assert(document.Succeeded, "library-style semantic analysis should remain supported");
        SemanticReachability reachability = document.Reachability
            ?? throw new InvalidOperationException("schema 6 semantic output omitted reachability");
        Assert(reachability.Mode == "all_callables_compatibility"
            && reachability.RootCallableIds.Count == 0,
            "inputs without exports should use the explicit compatibility mode");
        Assert(reachability.ReachableCallableIds.Count == document.Callables.Count
            && reachability.ReachableImports.Single().Name == "host_probe",
            "compatibility mode should preserve every projected callable and import");
    }

    private static void PropertyWriteRetainsOnlySetterAccessor()
    {
        AssertPropertyAccessorImports(
            "actor.Value = 2.0f;",
            new[] { "avid_property_set_value" },
            "property writes");
    }

    private static void PropertyReadRetainsOnlyGetterAccessor()
    {
        AssertPropertyAccessorImports(
            "_ = actor.Value;",
            new[] { "avid_property_get_value" },
            "property reads");
    }

    private static void PropertyReadModifyWriteRetainsBothAccessors()
    {
        AssertPropertyAccessorImports(
            "actor.Value += 2.0f;",
            new[] { "avid_property_get_value", "avid_property_set_value" },
            "property read-modify-writes");
    }

    private static void AssertPropertyAccessorImports(
        string operation,
        string[] expectedImports,
        string scenario)
    {
        string source = $$"""
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void BeginPlay()
                {
                    ActorProxy actor = default;
                    {{operation}}
                }
            }
            """;
        const string generatedSource = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct ActorProxy
            {
                public float Value
                {
                    get => Native.GetValue();
                    set => Native.SetValue(value);
                }
            }

            internal static class Native
            {
                [DllImport("avidscript", EntryPoint = "avid_property_get_value")]
                internal static extern float GetValue();

                [DllImport("avidscript", EntryPoint = "avid_property_set_value")]
                internal static extern void SetValue(float value);
            }
            """;
        const string sourceId = "Scripts/PropertyAccessorReachability.cs";
        string sourceHash = FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256;
        SemanticDocument document = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            sourceHash,
            new[]
            {
                new SemanticReferenceSource(
                    generatedSource,
                    "generated://AvidScript.PropertyBindings.generated.cs",
                    IsExecutable: true),
            });

        Assert(document.Succeeded, $"{scenario} should analyze");
        SemanticReachability reachability = document.Reachability
            ?? throw new InvalidOperationException($"{scenario} omitted reachability");
        string[] actualImports = reachability
            .ReachableImports
            .Select(import => import.Name)
            .ToArray();
        Assert(actualImports.SequenceEqual(expectedImports),
            $"{scenario} should authorize exactly [{string.Join(", ", expectedImports)}], " +
            $"but authorized [{string.Join(", ", actualImports)}]");

        bool expectsGetter = expectedImports.Contains("avid_property_get_value", StringComparer.Ordinal);
        bool expectsSetter = expectedImports.Contains("avid_property_set_value", StringComparer.Ordinal);
        string[] expectedAccessorIds = document.Callables
            .Where(callable => callable.AssociatedSymbolId is not null
                && ((!string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal) && expectsGetter)
                    || (string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
                        && callable.Parameters.Count > 0
                        && expectsSetter)))
            .Select(callable => callable.MethodSymbolId)
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToArray();
        string[] allAccessorIds = document.Callables
            .Where(callable => callable.AssociatedSymbolId is not null)
            .Select(callable => callable.MethodSymbolId)
            .ToArray();
        string[] actualAccessorIds = reachability.ReachableCallableIds
            .Where(allAccessorIds.Contains)
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToArray();
        Assert(actualAccessorIds.SequenceEqual(expectedAccessorIds),
            $"{scenario} should retain the exact accessor MethodSymbolIds");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

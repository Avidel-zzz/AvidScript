using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticReachabilityTests
{
    public static int Run()
    {
        ExportRootsRetainOnlyReachableFacadeAndPropertyImports();
        InputsWithoutExportsPreserveAllCallablesForAnalysis();
        return 2;
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
        Assert(document.SchemaVersion == 5 && document.SemanticVersion == "1.5",
            "reachability requires the explicit semantic schema version");
        SemanticReachability reachability = document.Reachability
            ?? throw new InvalidOperationException("schema 5 semantic output omitted reachability");
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
            ?? throw new InvalidOperationException("schema 5 semantic output omitted reachability");
        Assert(reachability.Mode == "all_callables_compatibility"
            && reachability.RootCallableIds.Count == 0,
            "inputs without exports should use the explicit compatibility mode");
        Assert(reachability.ReachableCallableIds.Count == document.Callables.Count
            && reachability.ReachableImports.Single().Name == "host_probe",
            "compatibility mode should preserve every projected callable and import");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

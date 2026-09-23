using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestGenericMethodTests
{
    public static int Run()
    {
        ClosedMethodsHaveIndependentFunctions();
        TamperedInstancesFailClosed();
        CompositeOpenTypesFailClosed();
        return 3;
    }

    private static void ClosedMethodsHaveIndependentFunctions()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                static T Identity<T>(T value) => value;
                static T Forward<T>(T value) => Identity<T>(value);
                static T Bounce<T>(int count, T value) =>
                    count == 0 ? value : Bounce<T>(count - 1, value);
                static U Second<T, U>(T first, U second) => second;

                [UnmanagedCallersOnly(EntryPoint = "generic_int")]
                public static int Int() => Identity<int>(7);
                [UnmanagedCallersOnly(EntryPoint = "generic_float")]
                public static float Float() => Identity<float>(2.5f);
                [UnmanagedCallersOnly(EntryPoint = "generic_forward")]
                public static int Forwarded() => Forward<int>(3);
                [UnmanagedCallersOnly(EntryPoint = "generic_bounce")]
                public static int Bounced() => Bounce<int>(2, 4);
                [UnmanagedCallersOnly(EntryPoint = "generic_pair")]
                public static int Pair() => Second<float, int>(2.5f, 9)
                    + (int)Second<int, float>(1, 3.5f);
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(semantic.Succeeded && semantic.SemanticVersion == "1.36",
            "closed generic source should publish the current semantic contract");
        SemanticCallable[] instances = semantic.Callables
            .Where(callable => callable.GenericDefinitionSymbolId is not null).ToArray();
        Check(instances.Length == 6 && instances.Select(callable => callable.MethodSymbolId)
            .Distinct(StringComparer.Ordinal).Count() == 6,
            "Identity, Forward, Bounce and ordered Second arguments need six distinct instances");
        Check(instances.All(callable => semantic.Reachability!.ReachableCallableIds
            .Contains(callable.MethodSymbolId))
            && semantic.Reachability!.ReachableCallableIds.All(id =>
                semantic.Callables.Single(callable => callable.MethodSymbolId == id)
                    .GenericTypeParameterIds?.Count == 0),
            "reachable functions must be closed and definition bodies must stay non-executable");
        CSharpGuestLoweringResult guest = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
        Check(guest.Succeeded,
            "closed generic methods must lower to Guest IR: "
            + string.Join(" | ", guest.Diagnostics.Select(diagnostic => diagnostic.Message)));
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(guest.Module!);
        Check(wasm.Succeeded, "closed generic Guest IR must compile to WASM");
        string? outputDirectory = Environment.GetEnvironmentVariable("AVIDSCRIPT_GENERIC_METHOD_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
            File.WriteAllBytes(Path.Combine(outputDirectory, "generic-methods.wasm"), wasm.Bytes);
        }
    }

    private static void TamperedInstancesFailClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                static T Identity<T>(T value) => value;
                [UnmanagedCallersOnly(EntryPoint = "generic_tamper")]
                public static int Main() => Identity<int>(7);
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(semantic.Succeeded, "tamper fixture must be valid");
        SemanticCallable instance = semantic.Callables.Single(callable =>
            callable.GenericDefinitionSymbolId is not null);
        SemanticDocument badId = semantic with
        {
            Callables = semantic.Callables.Select(callable => callable == instance
                ? callable with { GenericArgumentTypeIds = new[] { "type:float32" } }
                : callable).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(badId, new string('a', 64)).Succeeded,
            "changing the ordered generic arguments without its instance identity must fail");
        Check(!CSharpGuestLowerer.Lower(
            semantic with { SemanticVersion = "1.35" }, new string('a', 64)).Succeeded,
            "an old version cannot claim a closed generic execution plan");
    }

    private static void CompositeOpenTypesFailClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                static T[] Echo<T>(T[] values) => values;
                [UnmanagedCallersOnly(EntryPoint = "generic_array")]
                public static int Main() => Echo<int>(new[] { 1, 2 }).Length;
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(!semantic.Succeeded && semantic.Diagnostics.Any(diagnostic =>
            diagnostic.Code == "ASCS1064"),
            "composite open types must fail until they have a structured closed layout");
    }

    private static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/GenericMethods.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

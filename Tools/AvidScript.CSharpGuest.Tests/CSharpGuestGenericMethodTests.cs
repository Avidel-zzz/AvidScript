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
        StructuredNamedShapesFailClosed();
        CompositeOpenTypesFailClosed();
        return 4;
    }

    private static void ClosedMethodsHaveIndependentFunctions()
    {
        string source = File.ReadAllText(FindFixture());
        SemanticDocument semantic = Analyze(source);
        Check(semantic.Succeeded && semantic.SemanticVersion == "1.37",
            "closed generic source should publish the current semantic contract");
        SemanticCallable[] instances = semantic.Callables
            .Where(callable => callable.GenericDefinitionSymbolId is not null).ToArray();
        Check(instances.Length == 12 && instances.Select(callable => callable.MethodSymbolId)
            .Distinct(StringComparer.Ordinal).Count() == 12,
            "scalar, ref/out, array and ordered nested value calls need twelve distinct instances");
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
            semantic with { SchemaVersion = 30, SemanticVersion = "1.35" },
            new string('a', 64)).Succeeded,
            "an old version cannot claim a closed generic execution plan");
        Check(CSharpGuestLowerer.Lower(
            semantic with { SchemaVersion = 30, SemanticVersion = "1.36" },
            new string('a', 64)).Succeeded,
            "schema 30 / semantic 1.36 closed method artifacts must remain readable");
        SemanticDocument arraySemantic = Analyze(File.ReadAllText(FindFixture()));
        SemanticCallable arrayInstance = arraySemantic.Callables.Single(callable =>
            callable.GenericDefinitionSymbolId?.Contains(".EchoArray", StringComparison.Ordinal) == true);
        SemanticDocument badArray = arraySemantic with
        {
            Callables = arraySemantic.Callables.Select(callable => callable == arrayInstance
                ? callable with { ReturnTypeId = "type:float32[]" }
                : callable).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(badArray, new string('a', 64)).Succeeded,
            "a closed array instance cannot claim a different return element layout");
        SemanticControlFlowGraph arrayGraph = arraySemantic.ControlFlowGraphs.Single(graph =>
            graph.MethodSymbolId == arrayInstance.MethodSymbolId);
        SemanticDocument openArray = arraySemantic with
        {
            ControlFlowGraphs = arraySemantic.ControlFlowGraphs.Select(graph =>
                graph != arrayGraph ? graph : graph with
                {
                    Blocks = graph.Blocks.Select(block => block.BranchValue is null
                        ? block : block with
                        {
                            BranchValue = block.BranchValue with { TypeId = "type:T[]" },
                        }).ToArray(),
                }).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(openArray, new string('a', 64)).Succeeded,
            "an instance CFG cannot smuggle an open array result");
        SemanticDocument cyclicShape = arraySemantic with
        {
            TypeShapes = arraySemantic.TypeShapes.Select(shape =>
                shape.TypeId == "type:T[]" ? shape with { ElementTypeId = "type:T[]" }
                    : shape).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(cyclicShape, new string('a', 64)).Succeeded,
            "a cyclic array shape must fail input validation");
    }

    private static void StructuredNamedShapesFailClosed()
    {
        SemanticDocument semantic = Analyze(File.ReadAllText(FindFixture()));
        Check(semantic.Succeeded, "named generic fixture must analyze");
        SemanticTypeShape forward = semantic.TypeShapes.Single(shape =>
            shape.TypeId == "type:global::Pair<int, float>");
        SemanticTypeShape reverse = semantic.TypeShapes.Single(shape =>
            shape.TypeId == "type:global::Pair<float, int>");
        Check(forward.GenericDefinitionTypeId == reverse.GenericDefinitionTypeId
            && forward.GenericArgumentTypeIds!.SequenceEqual(new[] { "type:int32", "type:float32" })
            && reverse.GenericArgumentTypeIds!.SequenceEqual(new[] { "type:float32", "type:int32" }),
            "ordered named generic arguments must be structural rather than text substitutions");

        SemanticDocument swapped = semantic with
        {
            TypeShapes = semantic.TypeShapes.Select(shape => shape == forward
                ? shape with { GenericArgumentTypeIds = reverse.GenericArgumentTypeIds }
                : shape).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(swapped, new string('a', 64)).Succeeded,
            "two named types cannot claim the same definition and ordered arguments");
        SemanticDocument missingArgument = semantic with
        {
            TypeShapes = semantic.TypeShapes.Select(shape => shape == forward
                ? shape with { GenericArgumentTypeIds = new[] { "type:missing", "type:float32" } }
                : shape).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(missingArgument, new string('a', 64)).Succeeded,
            "a named shape cannot reference an unregistered argument type");
        SemanticDocument cyclic = semantic with
        {
            TypeShapes = semantic.TypeShapes.Select(shape => shape == forward
                ? shape with { GenericArgumentTypeIds = new[] { forward.TypeId, "type:float32" } }
                : shape).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(cyclic, new string('a', 64)).Succeeded,
            "a named shape cannot embed itself as an argument");
        SemanticDocument downgraded = semantic with
        {
            SchemaVersion = 30,
            SemanticVersion = "1.36",
        };
        Check(!CSharpGuestLowerer.Lower(downgraded, new string('a', 64)).Succeeded,
            "schema 30 cannot claim schema 31 named shape metadata");
    }

    private static void CompositeOpenTypesFailClosed()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                static T[,] Echo<T>(T[,] values) => values;
                [UnmanagedCallersOnly(EntryPoint = "generic_array")]
                public static int Main() => Echo<int>(new int[1, 1]).Length;
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Check(!semantic.Succeeded && semantic.Diagnostics.Any(diagnostic =>
            diagnostic.Code == "ASCS1064"),
            "unsupported composite open types must fail until they have a structured closed layout");
    }

    private static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/GenericMethods.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
    }

    private static string FindFixture()
    {
        for (DirectoryInfo? directory = new(AppContext.BaseDirectory);
            directory is not null; directory = directory.Parent)
        {
            string candidate = Path.Combine(
                directory.FullName, "Fixtures", "Phase66", "GenericMethods.cs");
            if (File.Exists(candidate))
                return candidate;
        }
        throw new FileNotFoundException("Fixtures/Phase66/GenericMethods.cs");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

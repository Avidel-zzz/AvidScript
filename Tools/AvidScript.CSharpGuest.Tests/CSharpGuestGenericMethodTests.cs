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
        GenericReferenceLayoutsFailClosed();
        CompositeOpenTypesFailClosed();
        return 5;
    }

    private static void ClosedMethodsHaveIndependentFunctions()
    {
        string source = File.ReadAllText(FindFixture());
        SemanticDocument semantic = Analyze(source);
        Check(semantic.Succeeded && semantic.SemanticVersion == "1.40",
            "closed generic source should publish the current semantic contract");
        SemanticCallable[] instances = semantic.Callables
            .Where(callable => callable.GenericDefinitionSymbolId is not null).ToArray();
        Check(instances.Length == 26 && instances.Select(callable => callable.MethodSymbolId)
            .Distinct(StringComparer.Ordinal).Count() == 26,
            $"scalar, ref/out, array, value and reference members need 26 distinct instances; found {instances.Length}");
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
        Check(CSharpGuestLowerer.Lower(
            semantic with { SemanticVersion = "1.37" },
            new string('a', 64)).Succeeded,
            "schema 31 / semantic 1.37 closed method artifacts must remain readable");
        Check(CSharpGuestLowerer.Lower(
            semantic with { SemanticVersion = "1.38" },
            new string('a', 64)).Succeeded,
            "schema 31 / semantic 1.38 closed member artifacts must remain readable");
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

    private static void GenericReferenceLayoutsFailClosed()
    {
        SemanticDocument semantic = Analyze(File.ReadAllText(FindFixture()));
        CSharpGuestLoweringResult guest = CSharpGuestLowerer.Lower(semantic, new string('a', 64));
        Check(guest.Succeeded, "closed generic reference fixture must lower");
        foreach ((string typeId, string fieldTypeId) in new[]
        {
            ("type:global::Box<int>", "type:int32"),
            ("type:global::Box<global::Pair<int, float>>", "type:global::Pair<int, float>"),
            ("type:global::MemberBox<int>", "type:int32"),
            ("type:global::MemberBox<global::Pair<int, float>>", "type:global::Pair<int, float>"),
        })
        {
            var reference = guest.Module!.Types.Single(type => type.Id == typeId);
            Check(reference.Kind == "managed_ref" && reference.ElementTypeId is not null,
                "each closed generic class needs its own managed reference layout");
            var payload = guest.Module.Types.Single(type => type.Id == reference.ElementTypeId);
            Check(payload.Fields.Count == 1 && payload.Fields[0].TypeId == fieldTypeId,
                "generic class state must use the ordered closed field type");
        }
        Check(!guest.Module!.Types.Any(type => type.Id == "type:global::Box<T>"),
            "the open class definition cannot become an executable managed reference");
        Check(!guest.Module.Types.Any(type => type.Id == "type:global::MemberBox<T>"),
            "the open class member definition cannot become an executable managed reference");
        SemanticDocument wrongDefinition = semantic with
        {
            TypeShapes = semantic.TypeShapes.Select(shape =>
                shape.TypeId == "type:global::Box<int>"
                    ? shape with { GenericDefinitionTypeId = "type:global::Pair<T, U>" }
                    : shape).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(wrongDefinition, new string('a', 64)).Succeeded,
            "a reference type cannot borrow another generic definition's field layout");

        SemanticCallable closedMember = semantic.Callables.Single(callable =>
            callable.GenericDefinitionSymbolId?.Contains("MemberBox<T>.Read", StringComparison.Ordinal) == true
            && callable.ContainingTypeId == "type:global::MemberBox<int>");
        SemanticDocument wrongMemberOwner = semantic with
        {
            Callables = semantic.Callables.Select(callable => callable == closedMember
                ? callable with { ContainingTypeId = "type:global::MemberBox<global::Pair<int, float>>" }
                : callable).ToArray(),
        };
        Check(!CSharpGuestLowerer.Lower(wrongMemberOwner, new string('a', 64)).Succeeded,
            "a closed member cannot claim another instantiation's receiver layout");
        Check(!CSharpGuestLowerer.Lower(
            semantic with { SemanticVersion = "1.37" }, new string('a', 64)).Succeeded,
            "semantic 1.37 cannot claim the new generic class member execution plan");
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

        const string shadowedParameter = """
            using System.Runtime.InteropServices;
            public sealed class Shadow<T>
            {
                public U Echo<U>(U value) => value;
                public T Echo<T>(T value, int ignored) => value;
            }
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "generic_shadow")]
                public static int Main() => new Shadow<int>().Echo<float>(2.5f, 0) > 0 ? 1 : 0;
            }
            """;
        SemanticDocument shadowed = Analyze(shadowedParameter);
        Check(!shadowed.Succeeded && shadowed.Diagnostics.Any(diagnostic =>
            diagnostic.Code == "ASCS1064"),
            "same-named class and method type parameters must fail closed until scoped identities exist");
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

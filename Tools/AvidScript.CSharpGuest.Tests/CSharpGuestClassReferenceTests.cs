using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestClassReferenceTests
{
    private static readonly string SemanticHash = new('c', 64);

    public static int Run()
    {
        GeneratedProjectClassLowersToNominalI32();
        ClassReferenceDoesNotImplicitlyConvert();
        return 2;
    }

    private static void GeneratedProjectClassLowersToNominalI32()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_class_ref_ordinal")]
                public static int GetProjectileOrdinal()
                {
                    TSubclassOfAActor value = ProjectClasses.ProjectileClass;
                    return value.AvidScriptOrdinal;
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            "generated class reference source should pass semantic analysis: " + FormatSemanticDiagnostics(semantic));
        string conversionEvidence = string.Join(" | ", semantic.Methods
            .SelectMany(method => Flatten(method.Root))
            .Where(operation => operation.Kind == "conversion")
            .Select(operation =>
                $"{operation.TypeId} <- {operation.Children.FirstOrDefault()?.TypeId}; "
                + $"kind={operation.Conversion?.Kind}; identity={operation.Conversion?.IsIdentity}; "
                + $"implicit={operation.Conversion?.IsImplicit}"));
        Assert(semantic.Methods
                .SelectMany(method => Flatten(method.Root))
                .Any(operation => operation.Kind == "conversion"
                    && operation.TypeId == "type:global::AvidScript.TSubclassOfAActor"
                    && operation.Children.Count == 1
                    && operation.Children[0].TypeId == operation.TypeId),
            "Roslyn should expose target-typed generated class construction as a same-type conversion: "
                + conversionEvidence);

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(FormatGuestDiagnostics(lowering));
        SemanticCallable intrinsicConstructor = semantic.Callables.Single(callable =>
            callable.IsConstructor
            && callable.ContainingTypeId == "type:global::AvidScript.TSubclassOfAActor");
        GuestType classReference = module.Types.Single(type =>
            type.Id == "type:global::AvidScript.TSubclassOfAActor");
        Assert(classReference.Kind == "class_ref"
            && classReference.Storage == "i32"
            && classReference.Size == 4
            && classReference.Alignment == 4
            && classReference.Fields.Count == 0,
            "TSubclassOfAActor should lower to one nominal i32 class_ref cell");
        Assert(module.Functions.All(function =>
                function.Id != "function:" + intrinsicConstructor.MethodSymbolId),
            "intrinsic class reference construction should not emit a Guest function");

        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        Assert(instructions.Any(instruction => instruction.Op == "constant"
                && instruction.Constant is { Kind: "class_ref", Value: "0" }),
            "ProjectClasses should construct its descriptor ordinal entirely inside the Guest");
        Assert(instructions.Any(instruction => instruction.Op == "convert"
                && instruction.OperatorKind == "class_ref_ordinal"),
            "the generated ABI getter should use the one sanctioned class_ref ordinal projection");
        Assert(GuestModuleValidator.Validate(module).Succeeded,
            "class reference Guest IR should pass independent validation");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8,
            "nominal class_ref i32 values should compile to WASM");
    }

    private static void ClassReferenceDoesNotImplicitlyConvert()
    {
        const string integerSource = """
            namespace AvidScript;
            public static class Script
            {
                public static int Invalid() => ProjectClasses.ProjectileClass;
            }
            """;
        const string actorSource = """
            namespace AvidScript;
            public static class Script
            {
                public static AActor Invalid() => ProjectClasses.ProjectileClass;
            }
            """;
        Assert(!Analyze(integerSource).Succeeded,
            "TSubclassOfAActor must not implicitly convert to int");
        Assert(!Analyze(actorSource).Succeeded,
            "TSubclassOfAActor must not implicitly convert to an object handle");
    }

    private static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/ClassReference.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[]
            {
                new SemanticReferenceSource(GeneratedReferenceSource, "generated://AvidScript.Bindings.generated.cs", true),
            });
    }

    private static string FormatSemanticDiagnostics(SemanticDocument document)
    {
        return string.Join(" | ", document.Diagnostics.Select(item => item.Code + ":" + item.Message));
    }

    private static string FormatGuestDiagnostics(CSharpGuestLoweringResult result)
    {
        return string.Join(" | ", result.Diagnostics.Select(item => item.Code + ":" + item.Message));
    }

    private static System.Collections.Generic.IEnumerable<SemanticOperation> Flatten(
        SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in Flatten(child))
            {
                yield return descendant;
            }
        }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private const string GeneratedReferenceSource = """
        using System.Runtime.InteropServices;

        namespace AvidScript;

        [StructLayout(LayoutKind.Sequential)]
        public readonly struct TSubclassOfAActor
        {
            private readonly int Ordinal;

            internal TSubclassOfAActor(int ordinal)
            {
                Ordinal = ordinal;
            }

            internal int AvidScriptOrdinal => Ordinal;
        }

        public readonly struct AActor
        {
            private readonly int Slot;
            private readonly int Generation;
        }

        public static class ProjectClasses
        {
            public static TSubclassOfAActor ProjectileClass => new(0);
        }
        """;
}

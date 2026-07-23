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
        GeneratedTypedClassLowersToNominalI32();
        ForgedClassReferencesAreRejected();
        ClassReferenceDoesNotImplicitlyConvert();
        LifecycleFacadeLowersToSharedImportsAndWasm();
        return 5;
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

    private static void GeneratedTypedClassLowersToNominalI32()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_typed_class_ref_ordinal")]
                public static int GetStaticMeshOrdinal()
                {
                    TSubclassOfAStaticMeshActor value = ProjectClasses.StaticMeshClass;
                    return value.AvidScriptOrdinal;
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            "generated typed class reference source should pass semantic analysis: " + FormatSemanticDiagnostics(semantic));

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(FormatGuestDiagnostics(lowering));
        GuestType classReference = module.Types.Single(type =>
            type.Id == "type:global::AvidScript.TSubclassOfAStaticMeshActor");
        Assert(classReference.Kind == "class_ref"
            && classReference.Storage == "i32"
            && classReference.Size == 4
            && classReference.Fields.Count == 0,
            "generated typed class references should lower to nominal i32 class_ref cells");
        Assert(module.Functions
                .SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .Any(instruction => instruction.Op == "constant"
                    && instruction.Constant is { Kind: "class_ref", Value: "1" }),
            "typed ProjectClasses entries should construct their ordinal inside the Guest");
    }

    private static void ForgedClassReferencesAreRejected()
    {
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly int Ordinal;\n    public TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }",
            "0"),
            "ASCG1004",
            "a public class reference constructor must not be treated as intrinsic");
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly int Ordinal;\n    internal TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }",
            "-1"),
            "ASCG1004",
            "negative class reference ordinals must be rejected");
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly int Ordinal;\n    internal TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }",
            "ordinal"),
            "ASCG1004",
            "nonliteral class reference ordinals must be rejected");
        AssertLoweringRejected(ForgedClassReferenceSource(
            "internal readonly int Ordinal;\n    internal TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }",
            "0"),
            "ASCG1003",
            "class reference ordinal fields must be private");
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly long Ordinal;\n    internal TSubclassOfAForged(int ordinal) { Ordinal = (long)ordinal; }",
            "0"),
            "ASCG1003",
            "class reference ordinal fields must be int32");
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly int Ordinal;\n    private readonly int Other;\n    internal TSubclassOfAForged(int ordinal) { Ordinal = ordinal; Other = 0; }",
            "0"),
            "ASCG1003",
            "look-alike class references must contain exactly one ordinal field");
    }

    private static void LifecycleFacadeLowersToSharedImportsAndWasm()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_lifecycle_probe")]
                public static int Probe()
                {
                    AActor actor = UE.SpawnActor(ProjectClasses.ProjectileClass, FTransform.Identity);
                    bool matches = UE.IsA(actor, ProjectClasses.ProjectileClass);
                    bool destroyed = UE.DestroyActor(actor);
                    return matches && destroyed ? 1 : 0;
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            "generated lifecycle facade should pass semantic analysis: " + FormatSemanticDiagnostics(semantic));

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(FormatGuestDiagnostics(lowering));
        Assert(lowering.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "generated lifecycle facade should lower to valid Guest IR");

        GuestImport spawn = module.Imports.Single(import => import.Name == "avid_object_spawn_actor");
        GuestImport destroy = module.Imports.Single(import => import.Name == "avid_object_destroy_actor");
        GuestImport isA = module.Imports.Single(import => import.Name == "avid_object_is_a");
        Assert(spawn.Module == "avidscript"
            && spawn.ParameterTypeIds.SequenceEqual(new[]
            {
                "type:int32", "type:address", "type:address",
            })
            && spawn.ReturnTypeId == "type:int32",
            "SpawnActor import should use class, transform address, handle out address, and status");
        Assert(destroy.Module == "avidscript"
            && destroy.ParameterTypeIds.SequenceEqual(new[] { "type:int32", "type:int32" })
            && destroy.ReturnTypeId == "type:int32",
            "DestroyActor import should use slot/generation and return status");
        Assert(isA.Module == "avidscript"
            && isA.ParameterTypeIds.SequenceEqual(new[] { "type:int32", "type:int32", "type:int32" })
            && isA.ReturnTypeId == "type:int32",
            "IsA import should use slot/generation/class ordinal and return bool storage");

        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
        Assert(instructions.Count(instruction => instruction.Op == "call"
                && module.Imports.Any(import => import.Id == instruction.TargetId)) >= 3,
            "public lifecycle wrappers should lower to the three raw host imports");
        Assert(instructions.Any(instruction => instruction.Op == "address_of"),
            "SpawnActor should pass one transform address and one handle out address without unsafe conversions");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8,
            "lifecycle Guest IR should compile to WASM");
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

    private static void AssertLoweringRejected(string source, string expectedCode, string message)
    {
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            message + ": semantic analysis should accept the source: " + FormatSemanticDiagnostics(semantic));
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        Assert(!lowering.Succeeded && lowering.Diagnostics.Any(item => item.Code == expectedCode),
            message + ": " + FormatGuestDiagnostics(lowering));
    }

    private static string ForgedClassReferenceSource(string members, string ordinal)
    {
        return $$"""
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct TSubclassOfAForged
            {
                {{members}}
                internal int AvidScriptOrdinal => (int)Ordinal;
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_forged_class_ref")]
                public static int Probe(int ordinal)
                {
                    TSubclassOfAForged value = new({{ordinal}});
                    return value.AvidScriptOrdinal;
                }
            }
            """;
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

        [StructLayout(LayoutKind.Sequential)]
        public readonly struct TSubclassOfAStaticMeshActor
        {
            private readonly int Ordinal;

            internal TSubclassOfAStaticMeshActor(int ordinal)
            {
                Ordinal = ordinal;
            }

            internal int AvidScriptOrdinal => Ordinal;
        }

        public readonly struct AActor
        {
            private readonly int Slot;
            private readonly int Generation;

            internal AActor(int slot, int generation)
            {
                Slot = slot;
                Generation = generation;
            }

            internal int AvidScriptSlot => Slot;
            internal int AvidScriptGeneration => Generation;
        }

        [StructLayout(LayoutKind.Sequential)]
        public readonly struct FVector
        {
            public readonly float X;
            public readonly float Y;
            public readonly float Z;

            public FVector(float x, float y, float z)
            {
                X = x;
                Y = y;
                Z = z;
            }

            public static FVector Zero => new(0.0f, 0.0f, 0.0f);
        }

        [StructLayout(LayoutKind.Sequential)]
        public readonly struct FRotator
        {
            public readonly float Pitch;
            public readonly float Yaw;
            public readonly float Roll;

            public FRotator(float pitch, float yaw, float roll)
            {
                Pitch = pitch;
                Yaw = yaw;
                Roll = roll;
            }

            public static FRotator Zero => new(0.0f, 0.0f, 0.0f);
        }

        [StructLayout(LayoutKind.Sequential)]
        public readonly struct FTransform
        {
            public readonly FVector Translation;
            public readonly FRotator Rotation;
            public readonly FVector Scale3D;

            public FTransform(FVector translation, FRotator rotation, FVector scale3D)
            {
                Translation = translation;
                Rotation = rotation;
                Scale3D = scale3D;
            }

            public static FTransform Identity => new(FVector.Zero, FRotator.Zero, new FVector(1.0f, 1.0f, 1.0f));
        }

        [StructLayout(LayoutKind.Sequential)]
        internal readonly struct FAvidScriptObjectHandle
        {
            internal readonly int Slot;
            internal readonly int Generation;
        }

        public static class ProjectClasses
        {
            public static TSubclassOfAActor ProjectileClass => new(0);
            public static TSubclassOfAStaticMeshActor StaticMeshClass => new(1);
        }

        public static class UE
        {
            public static AActor SpawnActor(TSubclassOfAActor actorClass, FTransform transform)
            {
                AvidScriptNative.SpawnActor(
                    actorClass.AvidScriptOrdinal,
                    in transform,
                    out FAvidScriptObjectHandle actorHandle);
                return new AActor(actorHandle.Slot, actorHandle.Generation);
            }

            public static bool DestroyActor(AActor actor)
                => AvidScriptNative.DestroyActor(actor.AvidScriptSlot, actor.AvidScriptGeneration) != 0;

            public static bool IsA(AActor actor, TSubclassOfAActor actorClass)
                => AvidScriptNative.IsA(
                    actor.AvidScriptSlot,
                    actor.AvidScriptGeneration,
                    actorClass.AvidScriptOrdinal) != 0;
        }

        internal static class AvidScriptNative
        {
            [DllImport("avidscript", EntryPoint = "avid_object_spawn_actor")]
            internal static extern int SpawnActor(
                int classOrdinal,
                in FTransform transform,
                out FAvidScriptObjectHandle actorHandle);

            [DllImport("avidscript", EntryPoint = "avid_object_destroy_actor")]
            internal static extern int DestroyActor(int slot, int generation);

            [DllImport("avidscript", EntryPoint = "avid_object_is_a")]
            internal static extern int IsA(int slot, int generation, int classOrdinal);
        }
        """;
}

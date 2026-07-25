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
        GeneratedTypedClassUpcastsNominally();
        DefaultObjectHandleLowersToZeroedMemory();
        ForgedClassReferencesAreRejected();
        AuthorizedClassReferenceOrdinalsFailClosed();
        ClassReferenceFieldOwnerMismatchFailsClosed();
        ClassReferenceConstructorOwnerMismatchFailsClosed();
        ClassReferenceTypeRequiresIntrinsicConstructor();
        ClassReferenceDoesNotImplicitlyConvert();
        LifecycleFacadeLowersToSharedImportsAndWasm();
        return 11;
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

    private static void GeneratedTypedClassUpcastsNominally()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_upcast_class_ref")]
                public static int Upcast()
                {
                    TSubclassOfAActor actorClass = ProjectClasses.StaticMeshClass;
                    return actorClass.AvidScriptOrdinal;
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            "generated class reference upcast should pass semantic analysis: "
                + FormatSemanticDiagnostics(semantic));

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(FormatGuestDiagnostics(lowering));
        GuestInstruction[] instructions = module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();

        Assert(lowering.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "generated class reference upcast should lower to valid Guest IR");
        Assert(instructions.Any(instruction => instruction.Op == "convert"
                && instruction.OperatorKind == "class_ref_upcast"),
            "generated class reference upcast should preserve the authorized ordinal");
        Assert(module.Functions.All(function =>
                !function.Id.Contains(".op_Implicit(", StringComparison.Ordinal)),
            "intrinsic class reference upcasts should not emit callable Guest bodies");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8,
            "class reference upcasts should compile to WASM as zero-cost i32 conversions");
    }

    private static void DefaultObjectHandleLowersToZeroedMemory()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_default_actor_slot")]
                public static int DefaultActorSlot()
                {
                    AActor actor = default;
                    return actor.AvidScriptSlot;
                }
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            "default object handle should pass semantic analysis: "
                + FormatSemanticDiagnostics(semantic));

        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(FormatGuestDiagnostics(lowering));
        Assert(lowering.Succeeded && GuestModuleValidator.Validate(module).Succeeded,
            "default object handle should lower to valid Guest IR");
        Assert(module.Functions
                .SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .Any(instruction => instruction.Op == "constant"
                    && instruction.Constant is { Kind: "zero", Value: null }),
            "default object handle should become an explicit zero-memory value");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8,
            "default object handle zero initialization should compile to WASM");
    }

    private static void ForgedClassReferencesAreRejected()
    {
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly int Ordinal;\n    public TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }",
            "0"),
            "ASCG1003",
            "a public class reference constructor must not be treated as intrinsic");
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly int Ordinal;\n    internal TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }",
            "-1"),
            "ASCG1003",
            "unauthorized class reference types must be rejected before ordinal validation");
        AssertLoweringRejected(ForgedClassReferenceSource(
            "private readonly int Ordinal;\n    internal TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }",
            "ordinal"),
            "ASCG1003",
            "unauthorized class reference types must not reach literal ordinal validation");
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
        AssertLoweringRejected(
            ForgedClassReferenceUpcastSource(),
            "ASCG1003",
            "a user-declared class reference cannot relabel an authorized ordinal through a forged upcast");
    }

    private static void AuthorizedClassReferenceOrdinalsFailClosed()
    {
        const string negativeSource = """
            using System.Runtime.InteropServices;
            namespace AvidScript;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_negative_class_ref")]
                public static TSubclassOfAStaticMeshActor Build() => new(-1);
            }
            """;
        AssertLoweringRejected(
            negativeSource,
            "ASCG1004",
            "negative ordinals on an authorized generated class reference must be rejected");

        const string nonLiteralSource = """
            using System.Runtime.InteropServices;
            namespace AvidScript;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_nonliteral_class_ref")]
                public static TSubclassOfAStaticMeshActor Build(int ordinal) => new(ordinal);
            }
            """;
        AssertLoweringRejected(
            nonLiteralSource,
            "ASCG1004",
            "nonliteral ordinals on an authorized generated class reference must be rejected");

        const string relabeledSource = """
            using System.Runtime.InteropServices;
            namespace AvidScript;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_relabeled_class_ref")]
                public static TSubclassOfAStaticMeshActor Build() => new(0);
            }
            """;
        AssertLoweringRejected(
            relabeledSource,
            "ASCG1004",
            "an authorized wrapper must not relabel an ordinal published for an incompatible wrapper");
    }

    private static void ClassReferenceTypeRequiresIntrinsicConstructor()
    {
        AssertLoweringRejected(PassiveForgedClassReferenceSource(
            "private readonly int Ordinal;"),
            "ASCG1003",
            "a class reference without an intrinsic constructor must be rejected even when never constructed");
        AssertLoweringRejected(PassiveForgedClassReferenceSource(
            "private readonly int Ordinal;\n    public TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }"),
            "ASCG1003",
            "a public constructor must not authorize a passive class reference type");
        AssertLoweringRejected(PassiveForgedClassReferenceSource(
            "private readonly int Ordinal;\n    internal TSubclassOfAForged(ref int ordinal) { Ordinal = ordinal; }"),
            "ASCG1003",
            "a ref constructor must not authorize a passive class reference type");
    }

    private static void ClassReferenceConstructorOwnerMismatchFailsClosed()
    {
        const string staticMeshTypeId = "type:global::AvidScript.TSubclassOfAStaticMeshActor";
        const string actorTypeId = "type:global::AvidScript.TSubclassOfAActor";
        SemanticDocument semantic = AnalyzeClassReferenceOwnerFixture();
        SemanticCallable build = semantic.Callables.Single(callable =>
            callable.Export?.Name == "avid_build_typed_class");
        SemanticCallable actorConstructor = semantic.Callables.Single(callable =>
            callable.IsConstructor
            && callable.ContainingTypeId == actorTypeId
            && callable.Parameters.Count == 1
            && callable.Parameters[0].TypeId == "type:int32");
        SemanticDocument constructorMismatch = WithReachableCallable(
            RewriteMethodOperations(
                semantic,
                build.MethodSymbolId,
                operation => operation.Kind == "object_creation"
                    && operation.TypeId == staticMeshTypeId
                        ? operation with { SymbolId = actorConstructor.MethodSymbolId }
                        : operation),
            actorConstructor.MethodSymbolId);
        AssertLoweringRejected(
            constructorMismatch,
            "ASCG1004",
            "class reference construction must use a constructor owned by the exact result type");
    }

    private static void ClassReferenceFieldOwnerMismatchFailsClosed()
    {
        const string staticMeshTypeId = "type:global::AvidScript.TSubclassOfAStaticMeshActor";
        const string actorTypeId = "type:global::AvidScript.TSubclassOfAActor";
        SemanticDocument semantic = AnalyzeClassReferenceOwnerFixture();
        string staticMeshOrdinalPropertyId = semantic.Symbols.Single(symbol =>
            symbol.Kind == "property"
            && symbol.Name == "AvidScriptOrdinal"
            && symbol.ContainingSymbolId == "symbol:" + staticMeshTypeId).Id;
        SemanticCallable staticMeshOrdinalGetter = semantic.Callables.Single(callable =>
            callable.AssociatedSymbolId == staticMeshOrdinalPropertyId);
        string actorOrdinalFieldId = semantic.Symbols.Single(symbol =>
            symbol.Kind == "field"
            && symbol.Name == "Ordinal"
            && symbol.ContainingSymbolId == "symbol:" + actorTypeId).Id;
        SemanticDocument fieldMismatch = RewriteMethodOperations(
            semantic,
            staticMeshOrdinalGetter.MethodSymbolId,
            operation => operation.Kind == "field_reference"
                && operation.Children.Count == 1
                && operation.Children[0].TypeId == staticMeshTypeId
                    ? operation with { SymbolId = actorOrdinalFieldId }
                    : operation);
        AssertLoweringRejected(
            fieldMismatch,
            "ASCG1004",
            "class reference ordinal fields must be owned by the exact receiver type");
    }

    private static SemanticDocument AnalyzeClassReferenceOwnerFixture()
    {
        const string source = """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_build_typed_class")]
                public static TSubclassOfAStaticMeshActor Build() => new(1);

                [UnmanagedCallersOnly(EntryPoint = "avid_read_typed_class")]
                public static int Read(TSubclassOfAStaticMeshActor value)
                    => value.AvidScriptOrdinal;
            }
            """;
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            "class reference owner mismatch fixture should pass semantic analysis: "
                + FormatSemanticDiagnostics(semantic));
        return semantic;
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
        AssertLoweringRejected(semantic, expectedCode, message);
    }

    private static void AssertLoweringRejected(
        SemanticDocument semantic,
        string expectedCode,
        string message)
    {
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        Assert(!lowering.Succeeded
            && lowering.Module is null
            && lowering.Diagnostics.Any(item => item.Code == expectedCode),
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

    private static string PassiveForgedClassReferenceSource(string members)
    {
        return $$"""
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct TSubclassOfAForged
            {
                {{members}}
                internal int AvidScriptOrdinal => Ordinal;
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_passive_forged_class_ref")]
                public static int Probe(TSubclassOfAForged value)
                    => value.AvidScriptOrdinal;
            }
            """;
    }

    private static string ForgedClassReferenceUpcastSource()
    {
        return """
            using System.Runtime.InteropServices;

            namespace AvidScript;

            public readonly struct TSubclassOfAForged
            {
                private readonly int Ordinal;
                internal TSubclassOfAForged(int ordinal) { Ordinal = ordinal; }
                public static implicit operator TSubclassOfAActor(TSubclassOfAForged value)
                    => new(value.Ordinal);
            }

            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_forged_class_ref_upcast")]
                public static int Probe()
                {
                    TSubclassOfAActor value = new TSubclassOfAForged(0);
                    return value.AvidScriptOrdinal;
                }
            }
            """;
    }

    private static SemanticDocument WithReachableCallable(
        SemanticDocument document,
        string methodSymbolId)
    {
        return document with
        {
            Reachability = document.Reachability! with
            {
                ReachableCallableIds = document.Reachability!.ReachableCallableIds
                    .Append(methodSymbolId)
                    .Distinct(StringComparer.Ordinal)
                    .OrderBy(id => id, StringComparer.Ordinal)
                    .ToArray(),
            },
        };
    }

    private static SemanticDocument RewriteMethodOperations(
        SemanticDocument document,
        string methodSymbolId,
        Func<SemanticOperation, SemanticOperation> rewrite)
    {
        return document with
        {
            ControlFlowGraphs = document.ControlFlowGraphs
                .Select(graph => graph.MethodSymbolId == methodSymbolId
                    ? graph with
                    {
                        Blocks = graph.Blocks.Select(block => block with
                        {
                            Operations = block.Operations
                                .Select(operation => RewriteOperation(operation, rewrite))
                                .ToArray(),
                            BranchValue = block.BranchValue is null
                                ? null
                                : RewriteOperation(block.BranchValue, rewrite),
                        }).ToArray(),
                    }
                    : graph)
                .ToArray(),
        };
    }

    private static SemanticOperation RewriteOperation(
        SemanticOperation operation,
        Func<SemanticOperation, SemanticOperation> rewrite)
    {
        SemanticOperation withChildren = operation with
        {
            Children = operation.Children
                .Select(child => RewriteOperation(child, rewrite))
                .ToArray(),
        };
        return rewrite(withChildren);
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

            public static implicit operator TSubclassOfAActor(TSubclassOfAStaticMeshActor value)
            {
                return new(value.Ordinal);
            }
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

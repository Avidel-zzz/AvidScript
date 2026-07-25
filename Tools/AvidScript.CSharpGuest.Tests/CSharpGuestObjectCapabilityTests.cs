using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestObjectCapabilityTests
{
    private static readonly string SemanticHash = new('d', 64);

    public static int Run()
    {
        GeneratedCapabilitiesLowerToNominalI32();
        FactoryFacadeLowersToPackedHandleImports();
        CapabilityForgeryFailsClosed();
        return 3;
    }

    private static void GeneratedCapabilitiesLowerToNominalI32()
    {
        GuestModule module = Lower(ValidUsageSource());
        GuestType factory = module.Types.Single(type =>
            type.Id == "type:global::AvidScript.TObjectFactoryOfInventoryState");
        GuestType objectType = module.Types.Single(type =>
            type.Id == "type:global::AvidScript.TObjectTypeOfSceneComponent");
        Assert(factory.Kind == "factory_ref"
            && factory.Storage == "i32"
            && factory.Size == 4
            && factory.Fields.Count == 0,
            "factory tokens should lower to nominal i32 values");
        Assert(objectType.Kind == "object_type_ref"
            && objectType.Storage == "i32"
            && objectType.Size == 4
            && objectType.Fields.Count == 0,
            "object type tokens should lower to a distinct nominal i32 family");

        GuestInstruction[] instructions = Instructions(module);
        Assert(instructions.Any(instruction => instruction.Op == "constant"
                && instruction.Constant is { Kind: "factory_ref", Value: "0" }),
            "ProjectFactories should publish the generated factory ordinal");
        Assert(instructions.Any(instruction => instruction.Op == "constant"
                && instruction.Constant is { Kind: "object_type_ref", Value: "3" }),
            "ProjectTypes should publish the generated object type ordinal");
        Assert(instructions.Any(instruction => instruction.Op == "convert"
                && instruction.OperatorKind == "factory_ref_ordinal")
            && instructions.Any(instruction => instruction.Op == "convert"
                && instruction.OperatorKind == "object_type_ref_ordinal"),
            "each token family should use only its sanctioned ordinal projection");
        Assert(GuestModuleValidator.Validate(module).Succeeded,
            "generated object capability Guest IR should pass independent validation");
    }

    private static void FactoryFacadeLowersToPackedHandleImports()
    {
        GuestModule module = Lower(ValidUsageSource());
        GuestImport construct = module.Imports.Single(import =>
            import.Module == "avidscript" && import.Name == "avid_object_construct");
        GuestImport release = module.Imports.Single(import =>
            import.Module == "avidscript" && import.Name == "avid_object_release");
        GuestImport find = module.Imports.Single(import =>
            import.Module == "avidscript" && import.Name == "avid_actor_find_component");
        Assert(construct.ParameterTypeIds.SequenceEqual(new[]
                { "type:int32", "type:int32", "type:int32" })
            && construct.ReturnTypeId == "type:int64",
            "construct should use the stable (i32,i32,i32)->i64 ABI");
        Assert(release.ParameterTypeIds.SequenceEqual(new[]
                { "type:int32", "type:int32" })
            && release.ReturnTypeId == "type:int32",
            "release should use the stable (i32,i32)->i32 ABI");
        Assert(find.ParameterTypeIds.SequenceEqual(new[]
                { "type:int32", "type:int32", "type:int32" })
            && find.ReturnTypeId == "type:int64",
            "find component should use the stable (i32,i32,i32)->i64 ABI");

        Dictionary<string, GuestRegister> values = module.Functions
            .SelectMany(function => function.Parameters.Concat(function.Locals))
            .GroupBy(register => register.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        GuestInstruction[] instructions = Instructions(module);
        Assert(instructions.Where(instruction => instruction.Op == "call"
                && (instruction.TargetId == construct.Id || instruction.TargetId == find.Id))
            .All(instruction => instruction.OperandIds.All(operandId =>
                values[operandId].TypeId == "type:int32")),
            "factory imports should pass cells directly without Guest scratch addresses");
        Assert(instructions.Any(instruction => instruction.Op == "binary"
                && instruction.OperatorKind == "right_shift")
            && instructions.Any(instruction => instruction.Op == "convert"
                && instruction.OperatorKind is null
                && instruction.OperandIds.Any(operandId =>
                    values.TryGetValue(operandId, out GuestRegister? operand)
                    && operand.TypeId == "type:int64")),
            "packed i64 handles should split into slot and generation in registers");

        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8,
            "object capability imports and packed handles should compile to WASM");
    }

    private static void CapabilityForgeryFailsClosed()
    {
        AssertLoweringRejected(
            """
            using System.Runtime.InteropServices;
            namespace AvidScript;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_forge_factory")]
                public static int Forge(UObject outer)
                {
                    TObjectFactoryOfInventoryState forged = new(0);
                    return UE.NewObject(outer, forged).AvidScriptSlot;
                }
            }
            """,
            "ASCG1004",
            "direct construction must not forge a generated factory token");
        AssertLoweringRejected(
            """
            using System.Runtime.InteropServices;
            namespace AvidScript;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_default_factory")]
                public static int DefaultFactory(UObject outer)
                {
                    TObjectFactoryOfInventoryState forged = default;
                    return UE.NewObject(outer, forged).AvidScriptSlot;
                }
            }
            """,
            "ASCG1004",
            "default must not manufacture factory ordinal zero");
        AssertLoweringRejected(
            """
            using System.Runtime.InteropServices;
            namespace AvidScript;
            public static class Script
            {
                private static TObjectFactoryOfInventoryState Forged;

                [UnmanagedCallersOnly(EntryPoint = "avid_static_factory")]
                public static int StaticFactory(UObject outer)
                    => UE.NewObject(outer, Forged).AvidScriptSlot;
            }
            """,
            "ASCG1003",
            "static zero initialization must not manufacture a nominal capability");
    }

    private static GuestModule Lower(string source)
    {
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            "object capability source should pass semantic analysis: "
                + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        return lowering.Module
            ?? throw new InvalidOperationException(string.Join(
                " | ",
                lowering.Diagnostics.Select(item => item.Code + ":" + item.Message)));
    }

    private static void AssertLoweringRejected(
        string source,
        string expectedCode,
        string message)
    {
        SemanticDocument semantic = Analyze(source);
        Assert(semantic.Succeeded,
            message + ": semantic analysis should accept the source");
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        Assert(!lowering.Succeeded
            && lowering.Module is null
            && lowering.Diagnostics.Any(diagnostic => diagnostic.Code == expectedCode),
            message + ": " + string.Join(" | ",
                lowering.Diagnostics.Select(item => item.Code + ":" + item.Message)));
    }

    private static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/ObjectCapabilities.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[]
            {
                new SemanticReferenceSource(
                    GeneratedReferenceSource,
                    "generated://AvidScript.Bindings.generated.cs",
                    true),
            });
    }

    private static GuestInstruction[] Instructions(GuestModule module)
    {
        return module.Functions
            .SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .ToArray();
    }

    private static string ValidUsageSource()
    {
        return """
            using System.Runtime.InteropServices;
            namespace AvidScript;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "avid_new_object")]
                public static int NewObject(UObject outer)
                {
                    UInventoryState value = UE.NewObject(
                        outer,
                        ProjectFactories.InventoryState);
                    int slot = value.AvidScriptSlot;
                    UE.Release(value);
                    return slot;
                }

                [UnmanagedCallersOnly(EntryPoint = "avid_component")]
                public static int Component(AActor actor)
                {
                    USceneComponent created = UE.CreateComponent(
                        actor,
                        ProjectFactories.SceneComponent);
                    USceneComponent found = UE.FindComponent(
                        actor,
                        ProjectTypes.SceneComponent);
                    UE.Release(created);
                    return found.AvidScriptGeneration;
                }
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

    private const string GeneratedReferenceSource = """
        using System.Runtime.InteropServices;

        namespace AvidScript;

        public readonly struct TObjectFactoryOfInventoryState
        {
            private readonly int Ordinal;
            internal TObjectFactoryOfInventoryState(int ordinal) { Ordinal = ordinal; }
            internal int AvidScriptOrdinal => Ordinal;
        }

        public readonly struct TObjectFactoryOfSceneComponent
        {
            private readonly int Ordinal;
            internal TObjectFactoryOfSceneComponent(int ordinal) { Ordinal = ordinal; }
            internal int AvidScriptOrdinal => Ordinal;
        }

        public readonly struct TObjectTypeOfSceneComponent
        {
            private readonly int Ordinal;
            internal TObjectTypeOfSceneComponent(int ordinal) { Ordinal = ordinal; }
            internal int AvidScriptOrdinal => Ordinal;
        }

        public static class ProjectFactories
        {
            public static TObjectFactoryOfInventoryState InventoryState => new(0);
            public static TObjectFactoryOfSceneComponent SceneComponent => new(1);
        }

        public static class ProjectTypes
        {
            public static TObjectTypeOfSceneComponent SceneComponent => new(3);
        }

        public readonly struct UObject
        {
            internal readonly int Slot;
            internal readonly int Generation;
            internal UObject(int slot, int generation) { Slot = slot; Generation = generation; }
            internal int AvidScriptSlot => Slot;
            internal int AvidScriptGeneration => Generation;
        }

        public readonly struct UInventoryState
        {
            internal readonly int Slot;
            internal readonly int Generation;
            internal UInventoryState(int slot, int generation) { Slot = slot; Generation = generation; }
            internal int AvidScriptSlot => Slot;
            internal int AvidScriptGeneration => Generation;
        }

        public readonly struct AActor
        {
            internal readonly int Slot;
            internal readonly int Generation;
            internal AActor(int slot, int generation) { Slot = slot; Generation = generation; }
            internal int AvidScriptSlot => Slot;
            internal int AvidScriptGeneration => Generation;
        }

        public readonly struct USceneComponent
        {
            internal readonly int Slot;
            internal readonly int Generation;
            internal USceneComponent(int slot, int generation) { Slot = slot; Generation = generation; }
            internal int AvidScriptSlot => Slot;
            internal int AvidScriptGeneration => Generation;
        }

        public static class UE
        {
            public static UInventoryState NewObject(
                UObject outer,
                TObjectFactoryOfInventoryState factory)
            {
                long packedHandle = AvidScriptNative.ObjectConstruct(
                    factory.AvidScriptOrdinal,
                    outer.AvidScriptSlot,
                    outer.AvidScriptGeneration);
                return new((int)packedHandle, (int)(packedHandle >> 32));
            }

            public static USceneComponent CreateComponent(
                AActor outer,
                TObjectFactoryOfSceneComponent factory)
            {
                long packedHandle = AvidScriptNative.ObjectConstruct(
                    factory.AvidScriptOrdinal,
                    outer.AvidScriptSlot,
                    outer.AvidScriptGeneration);
                return new((int)packedHandle, (int)(packedHandle >> 32));
            }

            public static USceneComponent FindComponent(
                AActor actor,
                TObjectTypeOfSceneComponent type)
            {
                long packedHandle = AvidScriptNative.ActorFindComponent(
                    actor.AvidScriptSlot,
                    actor.AvidScriptGeneration,
                    type.AvidScriptOrdinal);
                return new((int)packedHandle, (int)(packedHandle >> 32));
            }

            public static bool Release(UInventoryState value)
                => AvidScriptNative.ObjectRelease(
                    value.AvidScriptSlot,
                    value.AvidScriptGeneration) != 0;

            public static bool Release(USceneComponent value)
                => AvidScriptNative.ObjectRelease(
                    value.AvidScriptSlot,
                    value.AvidScriptGeneration) != 0;
        }

        internal static class AvidScriptNative
        {
            [DllImport("avidscript", EntryPoint = "avid_object_construct")]
            internal static extern long ObjectConstruct(
                int factoryOrdinal,
                int outerSlot,
                int outerGeneration);

            [DllImport("avidscript", EntryPoint = "avid_object_release")]
            internal static extern int ObjectRelease(int slot, int generation);

            [DllImport("avidscript", EntryPoint = "avid_actor_find_component")]
            internal static extern long ActorFindComponent(
                int actorSlot,
                int actorGeneration,
                int typeOrdinal);
        }
        """;
}

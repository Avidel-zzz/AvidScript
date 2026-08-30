using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestUeTypeTests
{
    private static readonly string SemanticHash = new('d', 64);

    public static int Run()
    {
        ScriptDefinedMethodCompilesToCanonicalWasmExport();
        ScriptDefinedPropertyCompilesToOrdinalImports();
        MultiUClassGuestOwnedStateFailsClosed();
        return 3;
    }

    private static void ScriptDefinedPropertyCompilesToOrdinalImports()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [UClass]
            public partial class Projectile : AvidActor
            {
                [UProperty]
                public float Damage { get; set; }

                [UFunction]
                public float ScaleDamage(float scale)
                {
                    Damage *= scale;
                    return Damage;
                }
            }

            [UClass]
            public partial class ExplosiveProjectile : Projectile
            {
                [UFunction]
                public float ReadInheritedDamage()
                {
                    return Damage;
                }
            }
            """;
        const string sourceId = "Scripts/Projectile.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://AvidScript.UeTypes.cs") });
        SemanticUePropertyRuntimePlan propertyPlan =
            SemanticUeTypeRuntimeContract.BuildPropertyPlans(semantic).Single();
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(string.Join(
                Environment.NewLine,
                lowering.Diagnostics.Select(diagnostic => diagnostic.Message)));
        GuestImport getter = module.Imports.Single(import =>
            import.Name == propertyPlan.GetterImportName);
        GuestImport setter = module.Imports.Single(import =>
            import.Name == propertyPlan.SetterImportName);
        SemanticUeFunctionDeclaration derivedMethod = semantic.UeTypeDeclarations
            .Single(type => type.TypeId == "type:global::Game.ExplosiveProjectile")
            .Functions.Single(function => function.Name == "ReadInheritedDamage");
        GuestExport derivedExport = module.Exports.Single(export =>
            export.Name == SemanticUeTypeRuntimeContract.GetFunctionExportName(
                derivedMethod.MethodSymbolId));
        GuestFunction derivedFunction = module.Functions.Single(function =>
            function.Id == derivedExport.FunctionId);
        CSharpGuestStateSchema stateSchema = CSharpGuestStateSchemaProjector.Project(
            semantic,
            module);
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);

        Assert(getter.Module == SemanticUeTypeRuntimeContract.HostModule
            && getter.ParameterTypeIds.SequenceEqual(new[] { propertyPlan.OwnerTypeId })
            && getter.ReturnTypeId == propertyPlan.ValueTypeId,
            "UPROPERTY getter should use the shared owner handle and ordinal import identity");
        Assert(setter.Module == SemanticUeTypeRuntimeContract.HostModule
            && setter.ParameterTypeIds.SequenceEqual(
                new[] { propertyPlan.OwnerTypeId, propertyPlan.ValueTypeId })
            && setter.ReturnTypeId == "type:void",
            "UPROPERTY setter should preserve native property value ownership");
        Assert(derivedFunction.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "convert")
            && derivedFunction.Blocks.SelectMany(block => block.Instructions).Any(instruction =>
                instruction.Op == "call" && instruction.TargetId == getter.Id),
            "derived script types should upcast their packed handle before inherited property access");
        Assert(lowering.Succeeded && wasm.Succeeded && wasm.Bytes.Length > 8,
            "ordinal UPROPERTY imports should compile to a real WASM module");
        Assert(stateSchema.OwnerTypeId == module.ModuleId && stateSchema.Slots.Count == 0,
            "multi-UClass modules with native-only instance state should publish an empty module state schema");
    }

    private static void MultiUClassGuestOwnedStateFailsClosed()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [UClass]
            public partial class FirstActor : AvidActor
            {
                private static int SharedState;

                [UFunction]
                public int ReadState() => SharedState;
            }

            [UClass]
            public partial class SecondActor : AvidActor
            {
                [UFunction]
                public int ReadValue() => 1;
            }
            """;
        const string sourceId = "Scripts/InvalidSharedState.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://AvidScript.UeTypes.cs") });
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException("multi-UClass state fixture should lower before schema ownership validation");
        bool rejected = false;
        try
        {
            CSharpGuestStateSchemaProjector.Project(semantic, module);
        }
        catch (System.IO.InvalidDataException exception)
        {
            rejected = exception.Message.Contains(
                "exactly one exported script owner",
                StringComparison.Ordinal);
        }

        Assert(rejected,
            "multi-UClass modules with mutable Guest-owned state must fail closed until per-instance schemas exist");
    }

    private static void ScriptDefinedMethodCompilesToCanonicalWasmExport()
    {
        const string source = """
            using AvidScript;

            namespace Game;

            [UClass]
            public partial class EncounterSubsystem : AvidWorldSubsystem
            {
                [UFunction]
                public int GetActiveEncounterCount()
                {
                    return 7;
                }
            }
            """;
        const string sourceId = "Scripts/EncounterSubsystem.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        SemanticDocument semantic = SemanticAnalyzer.Analyze(
            source,
            sourceId,
            frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(Facade, "generated://AvidScript.UeTypes.cs") });
        SemanticUeFunctionDeclaration function = semantic.UeTypeDeclarations
            .Single().Functions.Single();
        string exportName = SemanticUeTypeRuntimeContract.GetFunctionExportName(
            function.MethodSymbolId);

        Assert(semantic.Succeeded
            && semantic.Reachability!.RootCallableIds.Contains(function.MethodSymbolId),
            "script-defined UE methods should be semantic reachability roots");
        CSharpGuestLoweringResult lowering = CSharpGuestLowerer.Lower(semantic, SemanticHash);
        GuestModule module = lowering.Module
            ?? throw new InvalidOperationException(string.Join(
                Environment.NewLine,
                lowering.Diagnostics.Select(diagnostic => diagnostic.Message)));
        GuestType instanceType = module.Types.Single(type =>
            type.Id == "type:global::Game.EncounterSubsystem");
        GuestExport export = module.Exports.Single(item => item.Name == exportName);
        GuestFunction target = module.Functions.Single(item => item.Id == export.FunctionId);
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);

        Assert(lowering.Succeeded
            && instanceType.Kind == "handle"
            && instanceType.Storage == "i64"
            && instanceType.Size == 8,
            "script-defined this should lower to the shared packed ObjectHandle ABI");
        Assert(target.Parameters.Count == 1
            && target.Parameters[0].TypeId == instanceType.Id
            && target.ReturnTypeId == "type:int32",
            "generated UE exports should preserve instance and return ABI types");
        Assert(wasm.Succeeded && wasm.Bytes.Length > 8,
            "script-defined UE method Guest IR should compile to a real WASM module");
    }

    private const string Facade = """
        using System;

        namespace AvidScript;

        [AttributeUsage(AttributeTargets.Class, Inherited = false, AllowMultiple = false)]
        public sealed class UClassAttribute : Attribute { }

        [AttributeUsage(AttributeTargets.Method, Inherited = false, AllowMultiple = false)]
        public sealed class UFunctionAttribute : Attribute { }

        [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, Inherited = false, AllowMultiple = false)]
        public sealed class UPropertyAttribute : Attribute { }

        public abstract class AvidActor
        {
            protected virtual void BeginPlay() { }
            protected virtual void Tick(float deltaSeconds) { }
            protected virtual void EndPlay() { }
        }

        public abstract class AvidActorComponent
        {
            protected virtual void BeginPlay() { }
            protected virtual void Tick(float deltaSeconds) { }
            protected virtual void EndPlay() { }
        }

        public abstract class AvidWorldSubsystem
        {
            protected virtual void Initialize() { }
            protected virtual void Tick(float deltaSeconds) { }
            protected virtual void Deinitialize() { }
        }

        public abstract class AvidGameInstanceSubsystem
        {
            protected virtual void Initialize() { }
            protected virtual void Deinitialize() { }
        }
        """;

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

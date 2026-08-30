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
        return 1;
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

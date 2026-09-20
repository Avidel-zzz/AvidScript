using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestUeMethodFrameTests
{
    public static int Run()
    {
        const string facade = """
            namespace AvidScript {
                [System.AttributeUsage(System.AttributeTargets.Class)] public sealed class UClassAttribute : System.Attribute { }
                [System.AttributeUsage(System.AttributeTargets.Method)] public sealed class UFunctionAttribute : System.Attribute { }
                public abstract class AvidActor { }
            }
            """;
        const string source = """
            using AvidScript;
            [UClass] public partial class BaseActor : AvidActor {
                private int BaseStep(ref int value) { return ++value; }
                [UFunction] public int BaseEntry() { int value = 3; return BaseStep(ref value); }
            }
            [UClass] public partial class ChildActor : BaseActor {
                private int ChildStep(ref int value) { return ++value; }
                private int OutStep(out int value) { value = 5; return value; }
                private void Add(in int increment, ref int value) { value += increment; }
                [UFunction] public int Entry() {
                    int value = 7, other;
                    Add(in value, ref value);
                    return ChildStep(ref value) + OutStep(out other) + base.BaseEntry();
                }
            }
            """;
        int count = 0;
        SemanticDocument document = Analyze(source);
        var lowered = CSharpGuestLowerer.Lower(document, new string('b', 64));
        Check(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowered.Module!;
        Check(WasmModuleCompiler.Compile(module).Succeeded, "plain ref/out/in methods require no artificial closure to get the framed ABI");
        Check(!GuestModuleValidator.Validate(module with { SchemaVersion = 7, IrVersion = "1.6" }).Succeeded,
            "roots-only configuration cannot be silently downgraded to IR 7");
        Check(GuestIrSerializer.Serialize(module).SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module)))),
            "IR 8 method frames round trip canonically");
        string erased = module.Types.Single(type => type.Kind == "managed_ref").Id;
        GuestFunction badBody = module.Functions[0];
        GuestModule badAllocation = module with { Functions = module.Functions.Select(function => function != badBody ? function : function with {
            Locals = function.Locals.Append(new GuestRegister("$invalid_alloc", erased)).ToArray(),
            Blocks = function.Blocks.Select(block => block.Id != function.EntryBlockId ? block : block with {
                Instructions = new[] { new GuestInstruction("managed_new", "$invalid_alloc", Array.Empty<string>(), null, null, null) }
                    .Concat(block.Instructions).ToArray() }).ToArray() }).ToArray() };
        Check(!GuestModuleValidator.Validate(badAllocation).Succeeded && !WasmModuleCompiler.Compile(badAllocation).Succeeded,
            "roots-only support does not allow erased object allocation");
        var types = module.Types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        var functions = module.Functions.ToDictionary(function => function.Id, StringComparer.Ordinal);
        GuestCallFrameLayout Layout(GuestFramedExport export) => GuestCallFrameLayout.Create(export, functions[export.FunctionId], types, module.FunctionReferences);
        GuestFramedExport[] byReference = module.FramedExports.Where(export => export.ParameterKinds.SequenceEqual(new[] { "value", "ref" })).ToArray();
        Check(byReference.Length == 2 && byReference.All(export =>
            types[functions[export.FunctionId].Parameters[1].TypeId].Kind == GuestBorrowedReference.Kind),
            "both nominal receiver types use traced ref descriptors");
        Check(Layout(byReference[0]).SignatureSha256 == Layout(byReference[1]).SignatureSha256,
            "same method signature on base and derived receivers has the same normalized frame signature");
        GuestFramedExport output = module.FramedExports.Single(export => export.ParameterKinds.SequenceEqual(new[] { "value", "out" }));
        Check(Layout(output).SignatureSha256 != Layout(byReference[0]).SignatureSha256, "ref and out remain different contracts after receiver normalization");
        GuestFramedExport procedure = module.FramedExports.Single(export => export.ParameterKinds.SequenceEqual(new[] { "value", "in", "ref" }));
        Check(functions[procedure.FunctionId].ReturnTypeId == "type:void"
            && functions[procedure.FunctionId].Blocks.Single().Terminator.ReturnValueId is null,
            "void methods retain a void result and readonly parameter mode");
        Check(module.FramedExports.All(export => !module.Exports.Any(raw => raw.FunctionId == export.FunctionId)),
            "private method adapters never become raw UFUNCTION exports");
        Check(module.FramedExports.All(export => functions[export.FunctionId].Blocks.Single().Instructions[0].Op == "convert"
            && functions[functions[export.FunctionId].Blocks.Single().Instructions[1].TargetId!].Blocks
                .SelectMany(block => block.Instructions).Any(instruction => instruction.TargetId?.StartsWith("import:$ue:receiver:", StringComparison.Ordinal) == true)),
            "normalized adapters still enter nominal guarded bodies");
        var second = CSharpGuestLowerer.Lower(document with { UeMethodCatalog = document.UeMethodCatalog! with {
            Methods = document.UeMethodCatalog.Methods.Reverse().ToArray() } }, new string('b', 64));
        Check(!second.Succeeded && second.Module is null && second.Diagnostics.Any(item => item.Code == "ASCG1001"),
            "noncanonical method catalog order is rejected before generating adapters");
        const string debugSource = """
            using AvidScript;
            [UClass] public partial class Actor : AvidActor {
                private static int value;
                private void Add() { value++; }
                private int Unused(ref int input) { return ++input; }
                [UFunction] public void Entry() { Add(); }
            }
            """;
        SemanticDocument debug = Analyze(debugSource);
        var instrumented = CSharpGuestLowerer.Lower(debug, new string('b', 64), enableDebugInstrumentation: true);
        Check(instrumented.Succeeded && instrumented.Module!.FramedExports.Count == 1
            && WasmModuleCompiler.Compile(instrumented.Module).Succeeded,
            "synchronous void debug entry composes with a framed helper; unreachable ref methods do not require roots");
        var legacy = CSharpGuestLowerer.Lower(debug with { SchemaVersion = 25, SemanticVersion = "1.29", UeMethodCatalog = null }, new string('b', 64));
        Check(legacy.Succeeded && legacy.Module!.FramedExports.Count == 0 && WasmModuleCompiler.Compile(legacy.Module).Succeeded,
            "legacy semantic input retains direct calls without inventing a method catalog");
        return count;

        SemanticDocument Analyze(string text)
        {
            const string path = "Scripts/UeMethodFrames.cs";
            return SemanticAnalyzer.Analyze(text, path, FrontendAnalyzer.Analyze(text, path).Source.Sha256,
                new[] { new SemanticReferenceSource(facade, "generated://UeMethodFramesFacade.cs") });
        }
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }
}

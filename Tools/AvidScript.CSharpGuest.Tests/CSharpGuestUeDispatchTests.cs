using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestUeDispatchTests
{
    public static int Run()
    {
        const string source = """
            using AvidScript;
            public class State { public int Value; }
            public struct Packet { public State Item; public int Sum; }
            public interface IChange { Packet Apply(State value, ref int left, ref int right); }
            public interface IRead { int Read(State value); }
            [UClass] public partial class ReceiverActor : AvidActor, IChange, IRead {
                private static ReceiverActor First;
                private static ReceiverActor Second;
                public virtual State Echo(State value) { value.Value += 1; return value; }
                public virtual Packet Apply(State value, ref int left, ref int right) {
                    left += 2; right *= 3; value.Value += 1;
                    Packet result = new Packet(); result.Item = value; result.Sum = 14; return result;
                }
                int IRead.Read(State value) => value.Value + 5;
                protected static State Bounce(State value) => First.Echo(value);
                [UFunction] public int GetScriptValue() {
                    if (First == null) { First = this; return 11; }
                    if (Second == null) { if (this == First) return 33; Second = this; return 22; }
                    State state = new State(); state.Value = 5; int shared = 4;
                    IChange target = Second;
                    IChange empty = null;
                    if (empty != null || target == empty || target != Second) return 0;
                    Packet result = target.Apply(state, ref shared, ref shared);
                    IRead reader = Second;
                    return result.Item == state && result.Sum == 21 && state.Value == 7
                        && shared == 20 && reader.Read(state) == 12 ? 511 :
                        result.Item == state && result.Sum == 14 && state.Value == 6
                        && shared == 18 && reader.Read(state) == 11 ? 255 : 0;
                }
            }
            [UClass] public partial class DerivedReceiver : ReceiverActor {
                private int Extra => Helper();
                private int Helper() => 7;
                public override Packet Apply(State value, ref int left, ref int right) {
                    Packet result = base.Apply(value, ref left, ref right);
                    left += 2; result.Sum += Extra; result.Item = Bounce(value); return result;
                }
            }
            """;
        const string facade = """
            namespace AvidScript {
                [System.AttributeUsage(System.AttributeTargets.Class)] public sealed class UClassAttribute : System.Attribute { }
                [System.AttributeUsage(System.AttributeTargets.Method)] public sealed class UFunctionAttribute : System.Attribute { }
                public abstract class AvidActor { }
            }
            """;
        int count = 0;
        void Check(bool value, string reason) { if (!value) throw new InvalidOperationException(reason); ++count; }
        Check(CSharpGuestBorrowedReferenceTests.Reference(source + facade + """
            public static class Script { public static int Run() {
                ReceiverActor a = new ReceiverActor(), b = new DerivedReceiver();
                if (a.GetScriptValue() != 11 || b.GetScriptValue() != 22) return 0;
                return a.GetScriptValue();
            } }
            """) == 511, "same-source .NET virtual/base/interface/alias reference");
        Check(CSharpGuestBorrowedReferenceTests.Reference(source + facade + """
            public static class Script { public static int Run() {
                ReceiverActor a = new ReceiverActor(), b = new ReceiverActor();
                if (a.GetScriptValue() != 11 || b.GetScriptValue() != 22) return 0;
                return a.GetScriptValue();
            } }
            """) == 255, "same-source .NET base target has distinct observable behavior");
        const string path = "Scripts/UeDispatch.cs";
        var frontend = FrontendAnalyzer.Analyze(source, path);
        var semantic = SemanticAnalyzer.Analyze(source, path, frontend.Source.Sha256,
            new[] { new SemanticReferenceSource(facade, "generated://UeDispatchFacade.cs") });
        Check(semantic.Succeeded, string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
        byte[] before = SemanticSerializer.Serialize(semantic);
        var lowered = CSharpGuestLowerer.Lower(semantic, new string('f', 64));
        Check(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        Check(before.SequenceEqual(SemanticSerializer.Serialize(semantic)), "derived dispatch reachability must not mutate source artifact");
        var module = lowered.Module!;
        Check(module.FramedExports.Count(export => export.HostDispatchTargets is not null) == 3,
            "virtual and interface calls have distinct dynamic routes");
        Check(module.Functions.Any(function => function.Id.Contains("DerivedReceiver.Helper", StringComparison.Ordinal)),
            "private dependency reachable only from override must be lowered");
        Check(module.FramedExports.Where(export => export.HostDispatchTargets is not null)
            .All(export => export.HostDispatchTargets!.Count == 2), "both registered types have exact implementation choices");
        var wasm = WasmModuleCompiler.Compile(module);
        Check(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        Check(WasmModuleCompiler.Compile(module, new(true, 4)).Succeeded, "dynamic source compiles with cooperative cancellation");
        var stressed = module with { Functions = module.Functions.Select(function => function with {
            Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
                ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) }
                : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        var stressWasm = WasmModuleCompiler.Compile(stressed);
        Check(stressWasm.Succeeded, "dynamic source supports collection after each allocation");
        void RejectView(string text, string message)
        {
            var input = SemanticAnalyzer.Analyze(text, path, FrontendAnalyzer.Analyze(text, path).Source.Sha256,
                new[] { new SemanticReferenceSource(facade, "generated://UeDispatchFacade.cs") });
            var rejected = CSharpGuestLowerer.Lower(input, new string('f', 64));
            Check(input.Succeeded && !rejected.Succeeded && rejected.Module is null
                && rejected.Diagnostics.Any(item => item.Code == "ASCG1024" && item.Message.Contains(message, StringComparison.Ordinal)),
                "unsupported interface identity combination must reject explicitly: " + string.Join(" | ", rejected.Diagnostics.Select(item => item.Message)));
        }
        RejectView(source.Replace("IChange target = Second;", "IChange target = Second; DerivedReceiver checkedTarget = (DerivedReceiver)target;", StringComparison.Ordinal),
            "interface views");
        RejectView(source.Replace("IRead reader = Second;", "IRead reader = Foreign;", StringComparison.Ordinal)
            .Replace("private static ReceiverActor First;", "private static ReceiverActor First; private static ManagedReader Foreign;", StringComparison.Ordinal)
            + " public class ManagedReader : IRead { public int Read(State value) => value.Value + 5; }", "interface views");
        RejectView(source.Replace("public interface IRead { int Read(State value); }", "public interface IRead { int Read(State value) => value.Value + 5; }", StringComparison.Ordinal)
            .Replace("int IRead.Read(State value) => value.Value + 5;", "", StringComparison.Ordinal), "registered UE implementations");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "csharp-ue-dispatch.wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-ue-dispatch-stress.wasm"), stressWasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-ue-dispatch.guest-ir.json"), GuestIrSerializer.Serialize(module));
            File.WriteAllBytes(Path.Combine(directory, "csharp-ue-dispatch.semantic.json"), before);
            File.WriteAllText(Path.Combine(directory, "csharp-ue-dispatch.json"), JsonSerializer.Serialize(new {
                types = semantic.UeTypeDeclarations.Select((type, ordinal) => new {
                    type_ordinal = ordinal, type_id = type.TypeId,
                    derived = type.TypeId.Contains("DerivedReceiver", StringComparison.Ordinal),
                    functions = type.Functions.Select(method => new {
                        member_ordinal = SemanticUeTypeRuntimeContract.BuildMemberOrdinals(type)[method.MethodSymbolId],
                        method_id = method.MethodSymbolId,
                        export_name = SemanticUeTypeRuntimeContract.GetFunctionExportName(method.MethodSymbolId)
                    }).ToArray()
                }).ToArray(),
                imports = module.Imports.Select(import => new { module = import.Module, name = import.Name }).ToArray()
            }));
        }
        return count;
    }
}

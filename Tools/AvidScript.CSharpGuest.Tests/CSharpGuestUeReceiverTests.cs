using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestUeReceiverTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using AvidScript;
            public delegate int Change(ref int value, out int other);
            public class State { public int Value; }
            public struct Packet { public State Item; public int Sum; }
            [UClass] public partial class ReceiverActor : AvidActor {
                public static int Count;
                private int Read() => ++Count;
                private int Pure() => 7;
                private Func<int> Capture() => () => { if (this == null) return 0; return ++Count; };
                private Func<ReceiverActor> Self() => () => this;
                private Func<int> Mixed(int seed) => () => Pure() + ++seed;
                private Func<int> Local() { int ReadLocal() => Pure(); return ReadLocal; }
                private int Mutate(ref int value, out int other) { value += Pure(); other = value + 1; return other + Pure(); }
                private Packet Transfer(State original, ref State first, out State second, ref int left, ref int right,
                    int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k, int l, long tail, double fraction) {
                    left += 2; right *= 3;
                    first = new State(); first.Value = 19;
                    second = new State(); second.Value = first.Value + 4;
                    Packet result = new Packet(); result.Item = original;
                    result.Sum = a + b + c + d + e + f + g + h + i + j + k + l + (int)tail + (int)fraction;
                    original.Value += first.Value;
                    return result;
                }
                private State Echo(State value, int depth) { if (depth == 0) return value; return Echo(value, depth - 1); }
                [UFunction] public int GetScriptValue() {
                    Count = 0;
                    int bits = 0;
                    Func<int> a = Read, b = Read;
                    if (a == b) bits += 1;
                    if (a() == 1 && b() == 2) bits += 2;
                    Func<int> list = a + b;
                    if (list() == 4) bits += 4;
                    if (list - b == a && list - Read == a) bits += 8;
                    Func<int> c = Capture(), d = Capture();
                    if (c == d && c() == 5 && d() == 6) bits += 16;
                    Func<int> mixed = Mixed(1);
                    if (mixed != Mixed(1) && mixed() == 9 && mixed() == 10) bits += 32;
                    Change mutate = Mutate;
                    int value = 1; int other;
                    if (mutate(ref value, out other) == 16 && value == 8 && other == 9) bits += 64;
                    if (Self()() == this) bits += 128;
                    for (int i = 0; i < 64; i++) { Func<int> pure = Pure; if (pure() != 7) return 0; }
                    if (Local()() == 7) bits += 256;
                    State original = new State(); original.Value = 5;
                    State alias = original; int shared = 4;
                    Packet packet = Transfer(original, ref alias, out alias, ref shared, ref shared,
                        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 19L, 23.5);
                    if (packet.Item != original || packet.Sum != 120 || original.Value != 9
                        || alias.Value != 4 || shared != 18 || Echo(original, 4) != original) return 0;
                    return bits;
                }
            }
            """;
        int count = 0;
        Check(CSharpGuestBorrowedReferenceTests.Reference(source + Facade
            + "public static class Script { public static int Run() => new ReceiverActor().GetScriptValue(); }") == 511,
            "UE receiver source has the ordinary .NET delegate/capture result");
        Compile(source, "csharp-ue-receiver", true);
        const string fault = """
            using System;
            using AvidScript;
            [UClass] public partial class ReceiverActor : AvidActor {
                public static int Count;
                private int Pure() => 7;
                [UFunction] public int GetScriptValue() {
                    ReceiverActor missing = null;
                    Func<int> callback = missing.Pure;
                    Count = 9;
                    return callback();
                }
            }
            """;
        Compile(fault, "csharp-ue-receiver-null", false);
        const string shared = """
            using System;
            using AvidScript;
            [UClass] public partial class ReceiverActor : AvidActor {
                public static int Count;
                [UProperty] public int Value;
                private int Read() => Value;
                [UFunction] public int GetScriptValue() {
                    Func<int> read = () => Read();
                    Value += ++Count;
                    return Value * 100 + read();
                }
            }
            """;
        Check(CSharpGuestBorrowedReferenceTests.Reference(shared + Facade
            + "public static class Script { public static int Run() { ReceiverActor a = new ReceiverActor(), b = new ReceiverActor(); "
            + "a.Value = 10; b.Value = 20; return a.GetScriptValue() + b.GetScriptValue() + a.GetScriptValue(); } }") == 4747,
            "two ordinary instances share static state while captures read the correct receiver");
        Compile(shared, "csharp-ue-shared-bindings", true);
        const string plainReferences = """
            using AvidScript;
            [UClass] public partial class ReceiverActor : AvidActor {
                private int Mutate(ref int left, out int right) { left += 2; right = left * 3; return left + right; }
                private void Add(in int amount, ref int value) { value += amount; }
                [UFunction] public int GetScriptValue() {
                    int value = 4;
                    int result = Mutate(ref value, out value);
                    if (result != 36 || value != 18) return 0;
                    Add(in value, ref value);
                    return value == 36 ? 511 : 0;
                }
            }
            """;
        Check(CSharpGuestBorrowedReferenceTests.Reference(plainReferences + Facade
            + "public static class Script { public static int Run() => new ReceiverActor().GetScriptValue(); }") == 511,
            "ordinary primitive references retain immediate alias writes without any closures");
        Compile(plainReferences, "csharp-ue-receiver-plain-ref", false);
        const string crossObjects = """
            using AvidScript;
            public class State { public int Value; }
            public struct Packet { public State Item; public int Sum; }
            [UClass] public partial class ReceiverActor : AvidActor {
                private static ReceiverActor First;
                private static ReceiverActor Second;
                private State Echo(State value, int depth) {
                    if (this != First) return null;
                    value.Value += 1;
                    return depth == 0 ? value : Echo(value, depth - 1);
                }
                private Packet Transfer(ReceiverActor other, State original, ref State first, out State second,
                    ref int left, ref int right, int a, int b, int c, int d, int e, int f, int g, int h,
                    int i, int j, int k, int l, long tail, double fraction) {
                    left += 2; right *= 3;
                    first = new State(); first.Value = 19;
                    second = new State(); second.Value = first.Value + 4;
                    original.Value += first.Value;
                    Packet result = new Packet(); result.Item = other.Echo(original, 3);
                    result.Sum = this == Second ? a + b + c + d + e + f + g + h + i + j + k + l + (int)tail + (int)fraction : -1;
                    return result;
                }
                [UFunction] public int GetScriptValue() {
                    if (First == null) { First = this; return 11; }
                    if (Second == null) { if (this == First) return 33; Second = this; return 22; }
                    State original = new State(); original.Value = 5;
                    State alias = original; int shared = 4;
                    Packet result = Second.Transfer(First, original, ref alias, out alias, ref shared, ref shared,
                        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 19L, 23.5);
                    return result.Item == original && result.Sum == 120 && original.Value == 13
                        && alias.Value == 4 && shared == 18 ? 511 : 0;
                }
            }
            """;
        Check(CSharpGuestBorrowedReferenceTests.Reference(crossObjects + Facade
            + "public static class Script { public static int Run() { ReceiverActor a = new ReceiverActor(), b = new ReceiverActor(); "
            + "if (a.GetScriptValue() != 11 || b.GetScriptValue() != 22) return 0; return a.GetScriptValue(); } }") == 511,
            "natural A to B to A calls preserve shared objects, mixed signatures and immediate ref/out aliasing in .NET");
        Compile(crossObjects, "csharp-ue-cross-objects", true);
        return count;

        void Compile(string text, string name, bool stress)
        {
            const string path = "Scripts/UeReceiver.cs";
            FrontendDocument frontend = FrontendAnalyzer.Analyze(text, path);
            SemanticDocument document = SemanticAnalyzer.Analyze(text, path, frontend.Source.Sha256,
                new[] { new SemanticReferenceSource(Facade, "generated://AvidScript.UeReceiverFacade.cs") });
            CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('e', 64));
            Check(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ": " + item.Message)));
            GuestModule module = lowered.Module!;
            Check(module.Imports.Count(import => import.Name == "avid_ue_receiver_0_require_v1") == 1,
                "receiver authority is one typed host import for the generated owner");
            Check(module.FramedExports.Count != 0 && module.FramedExports.All(export =>
                module.Functions.Single(function => function.Id == export.FunctionId).Parameters[0].TypeId == "type:uint64"),
                "ordinary C# instance calls and bound method thunks use framed adapters with normalized weak receivers");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes),
                "UE receiver module emits deterministic WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            Check(!CSharpGuestLowerer.Lower(document, new string('e', 64), enableDebugInstrumentation: true).Succeeded,
                "UE closure references cannot enter unrooted debug pause frames");
            string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
                SemanticUeFunctionDeclaration entry = document.UeTypeDeclarations.Single().Functions.Single();
                var declaration = document.UeTypeDeclarations.Single();
                var ordinals = SemanticUeTypeRuntimeContract.BuildMemberOrdinals(declaration);
                File.WriteAllText(Path.Combine(directory, name + ".json"), JsonSerializer.Serialize(new {
                    type_id = document.UeTypeDeclarations.Single().TypeId,
                    method_id = entry.MethodSymbolId,
                    member_ordinal = ordinals[entry.MethodSymbolId],
                    export_name = SemanticUeTypeRuntimeContract.GetFunctionExportName(entry.MethodSymbolId),
                    properties = SemanticUeTypeRuntimeContract.BuildPropertyPlans(document).Select(plan => new {
                        member_ordinal = plan.MemberOrdinal,
                        stable_member_id = plan.PropertySymbolId,
                        name = declaration.Properties.Single(property => property.SymbolId == plan.PropertySymbolId).Name,
                        getter_import_name = plan.GetterImportName,
                        setter_import_name = plan.SetterImportName
                    }).ToArray(),
                    imports = module.Imports.Select(import => new { module = import.Module, name = import.Name }).ToArray()
                }));
            }
            if (!stress) return;
            GuestModule stressed = module with { Functions = module.Functions.Select(function => function with {
                Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
                    ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) }
                    : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
            var stressWasm = WasmModuleCompiler.Compile(stressed);
            Check(stressWasm.Succeeded, "UE receiver identity boxes and captures survive instrumented collection: "
                + string.Join(" | ", stressWasm.Diagnostics.Select(item => item.Message)));
            if (!string.IsNullOrWhiteSpace(directory)) File.WriteAllBytes(Path.Combine(directory, name + "-stress.wasm"), stressWasm.Bytes);
        }
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }

    private const string Facade = """
        namespace AvidScript {
            [System.AttributeUsage(System.AttributeTargets.Class)] public sealed class UClassAttribute : System.Attribute { }
            [System.AttributeUsage(System.AttributeTargets.Method)] public sealed class UFunctionAttribute : System.Attribute { }
            [System.AttributeUsage(System.AttributeTargets.Field | System.AttributeTargets.Property)] public sealed class UPropertyAttribute : System.Attribute { }
            public abstract class AvidActor { }
        }
        """;
}

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
            [UClass] public partial class ReceiverActor : AvidActor {
                public static int Count;
                private int Read() => ++Count;
                private int Pure() => 7;
                private Func<int> Capture() => () => { if (this == null) return 0; return ++Count; };
                private Func<ReceiverActor> Self() => () => this;
                private Func<int> Mixed(int seed) => () => Pure() + ++seed;
                private Func<int> Local() { int ReadLocal() => Pure(); return ReadLocal; }
                private int Mutate(ref int value, out int other) { value += Pure(); other = value + 1; return other + Pure(); }
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
                File.WriteAllText(Path.Combine(directory, name + ".json"), JsonSerializer.Serialize(new {
                    type_id = document.UeTypeDeclarations.Single().TypeId,
                    method_id = entry.MethodSymbolId,
                    export_name = SemanticUeTypeRuntimeContract.GetFunctionExportName(entry.MethodSymbolId),
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
            public abstract class AvidActor { }
        }
        """;
}

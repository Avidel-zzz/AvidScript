using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestClosureExecutionTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public static class Script {
                public static int Result;
                static Func<int> Make(int seed) {
                    int shared = seed;
                    int Read() => ++shared;
                    Func<int> first = Read;
                    Func<int> second = () => { shared += 10; return first(); };
                    first(); shared += 2;
                    return second;
                }
                static int Loops() {
                    Func<int> first = () => -1;
                    Func<int> last = () => -1;
                    for (int i = 0; i < 4; i++) {
                        int copy = i;
                        Func<int> read = () => i * 10 + copy;
                        if (i == 0) first = read;
                        last = read;
                    }
                    return first() * 100 + last();
                }
                static Func<int> Nested(int seed) {
                    Func<int, Func<int>> factory = value => () => value + seed;
                    return factory(3);
                }
                static int Recursive() {
                    Func<int, int> factorial = null;
                    factorial = n => n <= 1 ? 1 : n * factorial(n - 1);
                    return factorial(5);
                }
                static int Edges() {
                    Func<int> selected = () => 0;
                    for (int i = 0; i < 5; i++) {
                        int copy = i;
                        Func<int> read = () => copy;
                        if (i == 1) continue;
                        selected = read;
                        if (i == 3) break;
                    }
                    return selected();
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() {
                    Func<int> one = Make(7);
                    Func<int> two = Make(100);
                    int a = one(); int b = one(); int c = two();
                    Result = a + b * 100 + c * 10000 + Loops() + Nested(5)() + Recursive() + Edges();
                }
            }
            """;
        SemanticDocument document = CSharpGuestLexicalCaptureTests.Analyze(source);
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('a', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowered.Module!;
        Require(module.MemoryLayout.StateSlots.Single().Offset == 16, "native closure fixture result slot");
        Require(module.Types.Any(type => type.Kind == "managed_ref" && type.ElementTypeId is null)
            && module.Functions.Any(function => function.Id.Contains("$closure:thunk", StringComparison.Ordinal)), "closure typed thunks and erased roots");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        Require(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "closure WASM deterministic");
        GuestModule stress = module with { Functions = module.Functions.Select(function => function with
        { Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
            ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        WasmCompilationResult stressed = WasmModuleCompiler.Compile(stress);
        Require(stressed.Succeeded, "collection after every closure allocation must compile");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "csharp-closures.wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-closures-stress.wasm"), stressed.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-closures.guest-ir.json"), GuestIrSerializer.Serialize(module));
        }
        int count = 6;
        Require(CSharpGuestLowerer.Lower(document, new string('a', 64), enableDebugInstrumentation: true)
            .Diagnostics.Any(item => item.Code == "ASCG1024"), "closure pause roots must not silently become ephemeral");
        count++;
        foreach (string body in new[]
        {
            "int value = 1; Func<int> read = () => value; Touch(ref value); return read();",
            "Cell value = new Cell(); Func<int> read = () => value.Value; value.Value = 2; return read();",
            "Outer value = new Outer(); Func<int> read = () => value.Child.Value; value.Child.Value = 2; return read();",
            "Cell value = new Cell(); Func<int> read = () => value.Value; value.Mutate(); return read();",
            "Cell value = new Cell(); Func<int> read = () => value.Value; Touch(ref value.Value); return read();",
            "int value = 1; Func<int> read = () => value; return read == null ? 0 : read();",
        })
        {
            string rejectedSource = "using System; public struct Cell { public int Value; public void Mutate() { Value++; } } "
                + "public struct Outer { public Cell Child; } public static class Script { static void Touch(ref int n) { n++; } public static int Run() { " + body + " } }";
            CSharpGuestLoweringResult rejected = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(rejectedSource), new string('a', 64));
            Require(rejected.Succeeded && WasmModuleCompiler.Compile(rejected.Module!).Succeeded,
                "shared borrow capability must match its declared boundary: " + body + " | " + string.Join(" | ", rejected.Diagnostics.Select(item => item.Message)));
            count++;
        }
        return count + CSharpGuestBorrowedReferenceTests.Run() + CSharpGuestDelegateIdentityTests.Run()
            + CSharpGuestDelegateListTests.Run() + CSharpGuestBoundDelegateTests.Run();
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}

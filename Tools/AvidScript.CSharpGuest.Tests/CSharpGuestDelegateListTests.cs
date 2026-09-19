using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestDelegateListTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public delegate int Change(ref int a, ref int b, out int value);
            public struct Cell { public int Value; public Cell(int value) { Value = value; } }
            public static class Script {
                public static int Result;
                static Func<int> Pick(ref int order, Func<int> value, int digit) { order = order * 10 + digit; return value; }
                static Func<int> Make(int value) { int Read() => value; return Read; }
                public static int Run() {
                    int bits = 0, trace = 0, value = 0;
                    int A() { trace = trace * 10 + 1; return ++value; }
                    int B() { trace = trace * 10 + 2; value += 10; return value; }
                    Func<int> a = A, b = B, nil = null;
                    Func<int> list = a + b + a;
                    if (list() == 12 && trace == 121 && value == 12) bits += 1;
                    if (a + nil == a && nil + a == a && nil + nil == null && nil - a == null && a - nil == a) bits += 2;
                    Func<int> snapshot = list;
                    list += b; list -= a + b;
                    if (list == a + b && snapshot == a + b + a) bits += 4;
                    if ((a + b) + a == a + (b + a) && a + b != b + a) bits += 8;
                    if (a + b + a + b + a - (a + b + a) == a + b) bits += 16;
                    if (a + b + a - (a + a) == a + b + a) bits += 32;
                    if (a + b - (a + b) == null && a + a - a == a && a - a == null) bits += 64;
                    int order = 0;
                    Func<int> picked = Pick(ref order, a, 1) + Pick(ref order, b, 2);
                    if (order == 12 && picked == a + b) bits += 128;
                    Func<int> current = a;
                    Func<int> Replace() { current = b; return a; }
                    current += Replace();
                    if (current == a + a) bits += 256;
                    order = 0;
                    Action handlers = null;
                    Action tail = () => { order = order * 10 + 2; };
                    Action head = () => { order = order * 10 + 1; handlers -= tail; };
                    handlers = head + tail;
                    handlers(); handlers();
                    if (order == 121) bits += 512;
                    int First(ref int x, ref int y, out int output) { x++; y *= 2; output = y; return 1; }
                    int Last(ref int x, ref int y, out int output) { x += 3; y *= 3; output = y; return y; }
                    Change first = First, last = Last, both = first + last;
                    int shared = 1;
                    if (both(ref shared, ref shared, out int output) == 21 && shared == 21 && output == 21) bits += 1024;
                    Func<int> longList = null;
                    for (int i = 0; i < 32; i++) longList += a;
                    trace = 0; value = 0;
                    if (longList() == 32 && value == 32) bits += 2048;
                    Func<int> fresh = A;
                    if ((fresh + b) == (a + b) && (fresh + b) - a == b) bits += 4096;
                    Func<int> otherActivation = Make(3), secondActivation = Make(3);
                    if (otherActivation + a != secondActivation + a) bits += 8192;
                    Func<Cell> small = () => new Cell(2);
                    Func<Cell> large = () => new Cell(9);
                    Func<Cell> cells = small + large;
                    if (cells().Value == 9) bits += 16384;
                    Func<int> longer = a + b + a;
                    if (a + b - longer == a + b && a + b - b == a && a + b - a == b) bits += 32768;
                    return bits;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        int count = Check(source, "csharp-delegate-list", 65535, true);
        const string staticSource = """
            using System;
            using System.Runtime.InteropServices;
            public delegate int Change(ref int value);
            public static class Script {
                public static int Result;
                static int One() => 1;
                static int Two() => 2;
                static int Add(ref int value) { value++; return value; }
                static int Multiply(ref int value) { value *= 3; return value; }
                public static int Run() {
                    int bits = 0;
                    Func<int> a = One, b = Two;
                    Func<int> chain = a + b;
                    if (chain() == 2) bits += 1;
                    chain -= b;
                    if (chain == a && chain() == 1) bits += 2;
                    Change add = Add, multiply = Multiply, run = add + multiply;
                    int value = 2;
                    if (run(ref value) == 9 && value == 9) bits += 4;
                    return bits;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        count += Check(staticSource, "csharp-delegate-list-static", 7, false);
        return count;
    }

    private static int Check(string source, string name, int expected, bool captured)
    {
        Require(CSharpGuestBorrowedReferenceTests.Reference(source) == expected, name + ": .NET oracle");
        var document = CSharpGuestLexicalCaptureTests.Analyze(source);
        Require((document.ClosureEnvironments.Count > 0) == captured, name + ": capture coverage");
        var lowered = CSharpGuestLowerer.Lower(document, new string('f', 64));
        Require(lowered.Succeeded, name + ": " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowered.Module!;
        Require(module.MemoryLayout.StateSlots.Single().Offset == 16 && module.Types.Any(type => type.Kind == "managed_ref"), name + ": managed layout");
        var wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        GuestModule stress = module with { Functions = module.Functions.Select(function => function with
        { Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
            ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        var stressed = WasmModuleCompiler.Compile(stress);
        Require(stressed.Succeeded && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), name + ": stress and determinism");
        Require(CSharpGuestLowerer.Lower(document, new string('f', 64), enableDebugInstrumentation: true)
            .Diagnostics.Any(item => item.Code == "ASCG1024"), name + ": persistent debug roots rejected");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, name + "-stress.wasm"), stressed.Bytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), GuestIrSerializer.Serialize(module));
        }
        return 7;
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}

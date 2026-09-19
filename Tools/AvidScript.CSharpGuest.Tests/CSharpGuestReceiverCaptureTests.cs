using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestReceiverCaptureTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public struct Value { public int Count; public int Next() => ++Count; }
            public class Node {
                public int Count;
                public Value NestedValue;
                public Func<int> Callback;
                public Node(int count) { Count = count; Callback = () => ++Count; }
                public Func<int> Bind() => () => ++Count;
                public Func<Node> Self() => () => this;
                public Func<int> BindLocal() { int Read() => ++Count; return Read; }
                public Func<int> Capture(int seed) { int local = seed; return () => Count += ++local; }
                public Func<int, Func<int>> Nested() => amount => () => Count += amount;
                public Func<int> Both(int seed) {
                    int local = seed;
                    int Step() { local++; return ++Count + local; }
                    Func<int> bound = Step;
                    Step();
                    return () => bound() + Step();
                }
                public Func<int> Reader { get { return () => Count; } }
                public int Delta { set { Callback = () => Count += value; } }
                public Func<int> Borrow() => () => Increment(ref Count) + NestedValue.Next();
                static int Increment(ref int value) => ++value;
                public Func<int, int> Recursive() {
                    int Walk(int depth) => depth == 0 ? Count : Walk(depth - 1) + 1;
                    return Walk;
                }
                public Func<int> Loop() {
                    Func<int> last = null;
                    for (int i = 0; i < 3; i++) {
                        int copy = i;
                        last = () => Count + copy;
                    }
                    return last;
                }
            }
            public static class Script {
                public static int Result;
                static Func<int> Escape() { Node node = new Node(40); Func<int> read = node.Bind(); node = null; return read; }
                public static int Run() {
                    int bits = 0;
                    Node node = new Node(3);
                    if (node.Callback() == 4 && node.Count == 4) bits += 1;
                    if (node.Self()() == node) bits += 2;
                    Func<int> a = node.Bind(), b = node.Bind();
                    if (a == b && a != new Node(4).Bind() && a() == 5 && b() == 6) bits += 4;
                    Func<int> list = a + b;
                    if (list() == 8 && list - node.Bind() == a && node.Count == 8) bits += 8;
                    Func<int> local = node.BindLocal();
                    if (local == node.BindLocal() && local() == 9) bits += 16;
                    Func<int> mixed = node.Capture(1);
                    if (mixed != node.Capture(1) && mixed() == 11 && mixed() == 14) bits += 32;
                    Func<int, Func<int>> factory = node.Nested();
                    Func<int> three = factory(3), four = factory(4);
                    if (three() == 17 && four() == 21) bits += 64;
                    Func<int> both = new Node(5).Both(1);
                    if (both() == 22 && both() == 30) bits += 128;
                    Func<int> escaped = Escape();
                    if (escaped() == 41 && escaped() == 42) bits += 256;
                    node.Delta = 5;
                    if (node.Callback() == 26 && node.Reader() == 26) bits += 512;
                    node.NestedValue.Count = 2;
                    if (node.Borrow()() == 30 && node.Count == 27 && node.NestedValue.Count == 3) bits += 1024;
                    if (node.Recursive()(4) == 31 && node.Loop()() == 29) bits += 2048;
                    Node alias = node;
                    Func<Node> self = node.Self(); node = new Node(100);
                    if (self() == alias && self() != node && a() == 28 && alias.Count == 28) bits += 4096;
                    return bits;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        Require(CSharpGuestBorrowedReferenceTests.Reference(source) == 8191, "receiver captures match .NET reference semantics");
        var document = CSharpGuestLexicalCaptureTests.Analyze(source);
        Require(document.ClosureEnvironments.Any(environment => environment.Cells.Any(cell => cell.Kind == "receiver")),
            "fixture must exercise explicit receiver ownership");
        var lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        GuestModule module = lowered.Module!;
        Require(module.MemoryLayout.StateSlots.Single().Offset == 16, "receiver fixture result slot");
        var wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes),
            "receiver WASM compiles deterministically: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        GuestModule stress = module with { Functions = module.Functions.Select(function => function with {
            Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
                ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        var stressed = WasmModuleCompiler.Compile(stress);
        Require(stressed.Succeeded, string.Join(" | ", stressed.Diagnostics.Select(item => item.Message)));
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "csharp-receiver-capture.wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-receiver-capture-stress.wasm"), stressed.Bytes);
        }
        Require(!CSharpGuestLowerer.Lower(document, new string('d', 64), enableDebugInstrumentation: true).Succeeded,
            "captured receivers cannot enter unrooted pause frames");
        return 7 + Fault(directory);
    }

    private static int Fault(string? directory)
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public class Node {
                public int Count;
                public Func<int> Bind() => () => { Script.Result = ++Count; Node missing = null; return missing.Count; };
            }
            public static class Script {
                public static int Result;
                public static int Run() { Node node = new Node(); node.Count = 6; return node.Bind()(); }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Run(); }
            }
            """;
        Require(CSharpGuestBorrowedReferenceTests.Reference(source, typeof(NullReferenceException)) == 7,
            "receiver callback fault preserves preceding writes in .NET");
        var lowered = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(source), new string('d', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        var wasm = WasmModuleCompiler.Compile(lowered.Module!);
        Require(wasm.Succeeded && lowered.Module!.MemoryLayout.StateSlots.Single().Offset == 16,
            string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllBytes(Path.Combine(directory, "csharp-receiver-capture-fault.wasm"), wasm.Bytes);
        return 3;
    }

    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}

using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestReferenceObjectTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public struct Value { public int Count; public int Next() => ++Count; }
            public class Node {
                public int Count;
                public Node Link;
                public Value Nested;
                public Func<int> Callback;
                public Node(int count) { Count = count; }
                public Node(Node other) { Count = other.Count; Link = other; }
                public Node() : this(3) {}
                public int Next() => ++Count;
                public int Read() => Count;
                public int Number { get => Count; set { Func<int> read = () => value; Count = read(); } }
                public int Apply(ref int delta, out int old) { old = Count; Count += delta; delta++; return Count; }
            }
            public class Empty { public int Read() => 17; }
            public class DefaultNode { public int Count; public DefaultNode Link; }
            public delegate int Edit(ref int delta, out int old);
            public static class Script {
                public static int Result;
                static Node Evaluate(Node node, ref int calls) { calls++; return node; }
                static int Replace(ref Node node) { node = new Node(90); return 7; }
                static void Set(ref Node node, Node value) { node = value; }
                static int Bump(ref int value) { value++; return value; }
                static int Nested(in Node node) => node.Nested.Next();
                static Func<int> Escape(int value) { Node node = new Node(value); return node.Next; }
                public static int Run() {
                    int bits = 0;
                    Node node = new Node(3), alias = node;
                    object erased = node;
                    Node constructed = new Node(new Node());
                    alias.Count = 5;
                    if (node == alias && node.Count == 5 && node != new Node(5) && (Node)erased == node
                        && constructed.Count == 3 && constructed.Link.Count == 3) bits += 1;
                    Func<int> a = node.Next, b = alias.Next;
                    if (a == b && a() == 6 && node.Count == 6) bits += 2;
                    Node other = new Node(6);
                    Func<int> c = other.Next;
                    if (a != c && (a + b) - b == a && (a + c)() == 7 && node.Count == 7) bits += 4;
                    int calls = 0;
                    Evaluate(node, ref calls).Count += Replace(ref node);
                    if (calls == 1 && alias.Count == 14 && node.Count == 90) bits += 8;
                    Evaluate(alias, ref calls).Count = Replace(ref node);
                    if (calls == 2 && alias.Count == 7) bits += 16;
                    int previous = Evaluate(alias, ref calls).Count++;
                    if (calls == 3 && previous == 7 && alias.Count == 8) bits += 32;
                    alias.Nested.Count = 2;
                    if (Nested(in alias) == 3 && Bump(ref alias.Nested.Count) == 4 && alias.Nested.Count == 4) bits += 64;
                    Set(ref node, alias); Set(ref node.Link, node);
                    if (node == alias && node.Link == node) bits += 128;
                    int captured = 20;
                    node.Callback = () => ++captured;
                    Func<int> keep = node.Callback;
                    node.Callback = null;
                    if (keep() == 21 && captured == 21) bits += 256;
                    Func<int> escaped = Escape(30);
                    if (escaped() == 31 && escaped() == 32) bits += 512;
                    Edit edit = alias.Apply;
                    int delta = 2;
                    if (edit(ref delta, out int old) == 10 && old == 8 && delta == 3 && alias.Count == 10) bits += 1024;
                    alias.Number += 5;
                    if (alias.Number == 15) bits += 2048;
                    Empty empty = new Empty(); Func<int> emptyA = empty.Read, emptyB = empty.Read;
                    if (emptyA == emptyB && emptyA() == 17 && empty != new Empty()) bits += 4096;
                    DefaultNode fresh = new DefaultNode();
                    if (fresh.Count == 0 && fresh.Link == null && default(DefaultNode) == null) bits += 8192;
                    Func<int> once = Evaluate(alias, ref calls).Next;
                    alias = null;
                    if (calls == 4 && once() == 16 && a() == 17 && node.Count == 17) bits += 16384;
                    return bits;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        Require(CSharpGuestBorrowedReferenceTests.Reference(source) == 32767, "reference object .NET oracle");
        var document = CSharpGuestLexicalCaptureTests.Analyze(source);
        var lowered = CSharpGuestLowerer.Lower(document, new string('c', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        GuestModule module = lowered.Module!;
        Require(module.Types.Any(type => type.Kind == "managed_ref" && type.Id == "type:global::Node")
            && module.MemoryLayout.StateSlots.Single().Offset == 16, "typed object representation and result slot");
        var wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        GuestModule stress = module with { Functions = module.Functions.Select(function => function with {
            Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
                ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        var stressed = WasmModuleCompiler.Compile(stress);
        Require(stressed.Succeeded, string.Join(" | ", stressed.Diagnostics.Select(item => item.Message)));
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "csharp-reference-object.wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-reference-object-stress.wasm"), stressed.Bytes);
        }
        int count = 5 + Faults(directory);
        foreach (string declaration in new[] {
            "public class Node { public virtual int Read() => 7; }",
            "public class Parent {} public class Node : Parent {}",
            "public interface IValue {} public class Node : IValue {}",
            "public class Node { public int Count { get; set; } }",
            "public class Node { ~Node() {} }",
            "public class Node { public static int Count = 7; }"
        })
        {
            var rejected = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(declaration
                + " public static class Script { public static int Run() { Node node = new Node(); return node == null ? 0 : 1; } }"), new string('c', 64));
            Require(!rejected.Succeeded && rejected.Module is null, "unsupported object construction or dispatch must fail closed: " + declaration);
            count++;
        }
        Require(!CSharpGuestLowerer.Lower(document, new string('c', 64), enableDebugInstrumentation: true).Succeeded,
            "reference objects cannot enter pause frames without persistent roots");
        count++;
        foreach (string sourceBoundary in new[] {
            "using System.Runtime.InteropServices; public class Node { public int Value; } public static class Script { [DllImport(\"env\")] static extern int Host(Node node); public static int Run() => Host(new Node()); }",
            "public class Node { public int Value; } public static class Script { public static Node Current; public static int Run() { Current = new Node(); return Current.Value; } }"
        })
        {
            var rejected = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(sourceBoundary), new string('c', 64));
            Require(!rejected.Succeeded && rejected.Module is null, "Host and persistent state lifetime must fail closed");
            count++;
        }
        var asyncBoundary = CSharpGuestContinuationTests.Analyze("""
            using System.Runtime.InteropServices;
            using AvidScript;
            public class Node { public int Value; }
            public static class Script {
                public static int Result;
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void Begin() { Node node = new Node(); await AvidContinuations.NextTickAsync(); Result = node.Value; }
            }
            """, "Scripts/ReferenceObjectAsyncBoundary.cs");
        var asyncLowered = CSharpGuestLowerer.Lower(asyncBoundary, new string('c', 64));
        Require(asyncLowered.Succeeded && asyncLowered.Module is { } asyncModule
            && asyncModule.Functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions)
                .Any(instruction => instruction.Op == GuestContinuationState.StoreOp)
            && WasmModuleCompiler.Compile(asyncModule).Succeeded,
            "reference objects must enter typed, rooted async state objects");
        return count + 1;
    }

    private static int Faults(string? directory)
    {
        int count = 0;
        foreach (var sample in new[] {
            ("method", "Node node = null; node.Ignore(Side());", 1, typeof(NullReferenceException)),
            ("bind", "Node node = null; Func<int> callback = node.Read; Result = 99; callback();", 0, typeof(ArgumentException)),
            ("field", "Node node = null; node.Count = Side();", 1, typeof(NullReferenceException)),
            ("compound", "Node node = null; node.Count += Side();", 0, typeof(NullReferenceException)),
            ("borrow", "Node node = null; Edit(ref node.Count, Side());", 0, typeof(NullReferenceException)),
            ("nested", "Node node = null; node.Nested.Count = Side();", 0, typeof(NullReferenceException)),
            ("cast", "object value = new Empty(); Node node = (Node)value;", 0, typeof(InvalidCastException))
        })
        {
            string source = """
                using System;
                using System.Runtime.InteropServices;
                public struct Value { public int Count; }
                public class Node { public int Count; public Value Nested; public int Ignore(int value) => 17; public int Read() => 17; }
                public class Empty {}
                public static class Script {
                    public static int Result;
                    static int Side() { Result++; return 7; }
                    static void Edit(ref int value, int next) { value = next; }
                    public static int Run() {
                """ + sample.Item2 + "return Result; } [UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void Begin() { Run(); } }";
            int observed;
            try { observed = CSharpGuestBorrowedReferenceTests.Reference(source, sample.Item4); }
            catch (Exception exception) { throw new InvalidOperationException(sample.Item1 + ": .NET fault oracle", exception); }
            Require(observed == sample.Item3,
                sample.Item1 + ": .NET failure and side-effect ordering");
            var lowered = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(source), new string('c', 64));
            Require(lowered.Succeeded, sample.Item1 + ": " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
            var wasm = WasmModuleCompiler.Compile(lowered.Module!);
            Require(wasm.Succeeded && lowered.Module!.MemoryLayout.StateSlots.Single().Offset == 16,
                sample.Item1 + ": " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            if (!string.IsNullOrWhiteSpace(directory)) File.WriteAllBytes(Path.Combine(directory, "csharp-reference-object-fault-" + sample.Item1 + ".wasm"), wasm.Bytes);
            count += 3;
        }
        return count;
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}

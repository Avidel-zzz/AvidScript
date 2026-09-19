using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestBoundDelegateTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public delegate int Edit(ref int delta, out int old);
            public struct Counter {
                public int Value;
                public Counter(int value) { Value = value; }
                public int Next() => ++Value;
                public int Apply(ref int delta, out int old) { old = Value; Value += delta; delta++; return Value; }
                public readonly int Read() => Value;
            }
            public struct Callback {
                public Func<int> Read;
                public Callback(Func<int> read) { Read = read; }
                public int Invoke() => Read();
                public void Clear() { Read = null; }
            }
            public struct Empty { public int Read() => 17; }
            public static class Script {
                public static int Result;
                static Func<int> Make(int value) { Counter c = new Counter(value); return c.Next; }
                static Func<int> Bind(ref Counter value) => value.Next;
                static Counter Evaluate(ref int calls) { calls++; return new Counter(10); }
                public static int Run() {
                    int bits = 0;
                    Counter value = new Counter(3);
                    Func<int> a = value.Next;
                    value.Value = 100;
                    if (a() == 4 && value.Value == 100) bits += 1;
                    Func<int> b = value.Next, separatelyBoxed = value.Next;
                    if (b != separatelyBoxed && b() == 101) bits += 2;
                    Func<int> copy = a;
                    if (copy == a && copy() == 5 && a() == 6) bits += 4;
                    Func<int> escaped = Make(9);
                    if (escaped() == 10 && escaped() == 11) bits += 8;
                    Counter borrowed = new Counter(2);
                    Func<int> borrowedCopy = Bind(ref borrowed);
                    borrowed.Value = 90;
                    if (borrowedCopy() == 3 && borrowed.Value == 90) bits += 16;
                    int calls = 0;
                    Func<int> once = Evaluate(ref calls).Next;
                    if (calls == 1 && once() == 11 && once() == 12 && calls == 1) bits += 32;
                    Counter original = new Counter(7);
                    Edit edit = original.Apply;
                    int delta = 2;
                    if (edit(ref delta, out int old) == 9 && old == 7 && delta == 3
                        && edit(ref delta, out old) == 12 && old == 9 && delta == 4 && original.Value == 7) bits += 64;
                    Func<int> combined = a + b;
                    if (combined() == 102 && combined - b == a && combined - separatelyBoxed == combined) bits += 128;
                    int captured = 5;
                    Func<int> inner = () => ++captured;
                    Callback wrapper = new Callback(inner);
                    Func<int> kept = wrapper.Invoke;
                    wrapper.Clear(); inner = null;
                    if (kept() == 6 && kept() == 7 && captured == 7) bits += 256;
                    Func<int> read = original.Read;
                    original.Value = 55;
                    if (read() == 7) bits += 512;
                    Func<int> explicitCreation = new Func<int>(original.Next);
                    if (explicitCreation() == 56 && original.Value == 55) bits += 1024;
                    Empty empty = new Empty();
                    Func<int> emptyA = empty.Read, emptyB = empty.Read;
                    if (emptyA != emptyB && emptyA() == 17) bits += 2048;
                    return bits;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        int count = Check(source, "csharp-bound-delegate", 4095, true);
        const string plain = """
            using System;
            using System.Runtime.InteropServices;
            public struct Counter {
                public int Value;
                public Counter(int value) { Value = value; }
                public int Next() => ++Value;
            }
            public static class Script {
                public static int Result;
                public static int Run() {
                    Counter value = new Counter(4);
                    Func<int> next = value.Next;
                    value.Value = 90;
                    return next() * 100 + next() * 10 + value.Value;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        count += Check(plain, "csharp-bound-delegate-plain", 650, false);
        foreach (string unsupported in new[] {
            "using System; public sealed class Receiver { public int Read() => 1; } public static class Script { public static int Run() { Receiver value = new Receiver(); Func<int> read = value.Read; return read(); } }",
            "using System; public struct Receiver { public int[] Values; public int Read() => Values[0]; } public static class Script { public static int Run() { Receiver value = new Receiver(); Func<int> read = value.Read; return read(); } }"
        })
        {
            var rejected = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(unsupported), new string('c', 64));
            Require(!rejected.Succeeded && rejected.Module is null, "unsupported reference identity or borrowed receiver fields must fail closed");
            count++;
        }
        return count;
    }

    private static int Check(string source, string name, int expected, bool hasClosures)
    {
        Require(CSharpGuestBorrowedReferenceTests.Reference(source) == expected, name + ": .NET oracle");
        var document = CSharpGuestLexicalCaptureTests.Analyze(source);
        Require((document.ClosureEnvironments.Count != 0) == hasClosures, name + ": receiver-only module coverage");
        var lowered = CSharpGuestLowerer.Lower(document, new string('c', 64));
        Require(lowered.Succeeded, name + ": " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowered.Module!;
        Require(module.MemoryLayout.StateSlots.Single().Offset == 16 && module.Types.Any(type => type.Id.Contains("$delegate:box:", StringComparison.Ordinal)), name + ": box and result layout");
        var wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded, name + ": " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        GuestModule stress = module with { Functions = module.Functions.Select(function => function with
        { Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
            ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        var stressed = WasmModuleCompiler.Compile(stress);
        Require(stressed.Succeeded && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), name + ": collection stress and deterministic output");
        Require(CSharpGuestLowerer.Lower(document, new string('c', 64), enableDebugInstrumentation: true)
            .Diagnostics.Any(item => item.Code == "ASCG1024"), name + ": pause roots reject unsupported lifetime");
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

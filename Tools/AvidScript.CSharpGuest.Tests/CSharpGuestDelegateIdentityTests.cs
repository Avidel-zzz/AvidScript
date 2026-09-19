using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestDelegateIdentityTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public static class Script {
                public static int Result;
                static int Static() => 3;
                static int Other() => 3;
                static Func<int> Make(int seed) { int Read() => seed; return Read; }
                static void Pair(int seed, out Func<int> a, out Func<int> b) {
                    for (int i = 0; i < 1; i++) {
                        int copy = 3;
                        int Read() => seed + copy;
                        a = Read; b = Read; return;
                    }
                    a = null; b = null;
                }
                static Func<int> Pick(ref int count, Func<int> value) { count++; return value; }
                public static int Run() {
                    int bits = 0;
                    Func<int> nil = null;
                    if (nil == null) bits += 1;
                    Func<int> a = Static; Func<int> b = Static; Func<int> c = Other;
                    if (a == b) bits += 2;
                    if (a != c) bits += 4;
                    if (a != null) bits += 8;
                    Func<int> first = Make(7); Func<int> second = Make(7);
                    if (first != second) bits += 16;
                    Func<int> copied = first;
                    if (copied == first) bits += 32;
                    int value = 4;
                    int Local() => value;
                    int Another() => value;
                    Func<int> x = Local; Func<int> y = Local; Func<int> z = Another;
                    if (x == y) bits += 64;
                    value++;
                    if (x == y) bits += 128;
                    if (x != z) bits += 256;
                    Func<int> one = null; Func<int> two = null;
                    for (int i = 0; i < 2; i++) {
                        int copy = 7; int Read() => copy;
                        if (i == 0) one = Read; else two = Read;
                    }
                    if (one != two) bits += 512;
                    Pair(3, out Func<int> pairA, out Func<int> pairB);
                    if (pairA == pairB) bits += 1024;
                    int count = 0;
                    if (Pick(ref count, x) == Pick(ref count, y) && count == 2) bits += 2048;
                    Func<int> lambda = () => value;
                    Func<int> anotherLambda = () => value;
                    if (lambda != anotherLambda) bits += 4096;
                    return bits;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        Require(CSharpGuestBorrowedReferenceTests.Reference(source) == 8191, ".NET delegate identity reference result");
        var document = CSharpGuestLexicalCaptureTests.Analyze(source);
        var lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowered.Module!;
        Require(module.MemoryLayout.StateSlots.Single().Offset == 16 && module.Functions.Any(function => function.Id.StartsWith("function:$delegate:equals:", StringComparison.Ordinal)),
            "delegate equality helper and result slot");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Require(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        GuestModule stress = module with { Functions = module.Functions.Select(function => function with
        { Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
            ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        WasmCompilationResult stressed = WasmModuleCompiler.Compile(stress);
        Require(stressed.Succeeded && compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "delegate identity stress and deterministic codegen");
        const string plainSource = """
            using System;
            using System.Runtime.InteropServices;
            public static class Script {
                public static int Result;
                static int One() => 1;
                static int Two() => 2;
                public static int Run() {
                    Func<int> a = One; Func<int> b = One; Func<int> c = Two;
                    int result = 0;
                    if (a == b) result += 1;
                    if (a != c) result += 2;
                    if (a != null) result += 4;
                    return result;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        Require(CSharpGuestBorrowedReferenceTests.Reference(plainSource) == 7, ".NET static delegate equality reference");
        var plain = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(plainSource), new string('e', 64));
        Require(plain.Succeeded && !plain.Module!.Types.Any(type => type.Kind == "managed_ref"), "static-only comparison retains function reference representation");
        WasmCompilationResult plainWasm = WasmModuleCompiler.Compile(plain.Module!);
        Require(plainWasm.Succeeded && plain.Module!.MemoryLayout.StateSlots.Single().Offset == 16, "static-only comparison compiles and exports result");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "csharp-delegate-identity.wasm"), compiled.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-delegate-identity-stress.wasm"), stressed.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-delegate-identity.guest-ir.json"), GuestIrSerializer.Serialize(module));
            File.WriteAllBytes(Path.Combine(directory, "csharp-delegate-identity-static.wasm"), plainWasm.Bytes);
        }
        return 8;
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}

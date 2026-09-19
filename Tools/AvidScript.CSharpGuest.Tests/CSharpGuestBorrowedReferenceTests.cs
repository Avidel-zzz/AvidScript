using System;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using AvidScript.CSharpGuest;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestBorrowedReferenceTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public delegate void RefCall(ref int a, ref int b, Func<int> read);
            public struct Cell {
                public int Value;
                public Cell(int value) { Value = value; }
                public void Mutate() { Value += 2; }
                public readonly int Read() { return Value; }
                public int Property { get { return Value; } set { Value = value; } }
            }
            public struct Outer { public Cell Child; }
            public static class Script {
                public static int Result;
                static void Pair(ref int a, ref int b, Func<int> read) { a += 3; b += read(); }
                static void Touch(ref int value) { value++; }
                static void TouchCell(ref Cell value) { value.Value += 3; value.Mutate(); }
                static int ReadonlyCell(in Cell value) { value.Mutate(); return value.Read(); }
                static void Replace(ref Func<int> value, out Func<int> copy) {
                    int seed = 100; value = () => ++seed; copy = value;
                }
                public static int Run() {
                    int shared = 2;
                    Func<int> read = () => shared;
                    Pair(ref shared, ref shared, read);
                    RefCall call = Pair;
                    call(ref shared, ref shared, read);
                    int step = 4;
                    RefCall closure = (ref int a, ref int b, Func<int> probe) => { a += step; b += probe(); };
                    closure(ref shared, ref shared, read);
                    Cell value = new Cell(5);
                    Func<int> valueRead = () => value.Value;
                    value.Value = 7; value.Mutate(); value.Property += 1;
                    Touch(ref value.Value); TouchCell(ref value);
                    Cell copy = value; copy.Mutate();
                    int readonlyResult = ReadonlyCell(in value);
                    Outer outer = new Outer();
                    Func<int> outerRead = () => outer.Child.Value;
                    outer.Child = new Cell(3); outer.Child.Mutate();
                    Touch(ref outer.Child.Value); TouchCell(ref outer.Child);
                    int first = read();
                    Replace(ref read, out Func<int> second);
                    return first + valueRead() * 100 + copy.Value + outerRead() * 10000 + read() + second() + readonlyResult;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void Begin() { Result = Run(); }
            }
            """;
        const int expected = 111897;
        Require(Reference(source) == expected, "ordinary .NET reference result");
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(source), new string('b', 64));
        Require(lowered.Succeeded, string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
        GuestModule module = lowered.Module!;
        Require(module.MemoryLayout.StateSlots.Single().Offset == 16, "borrowed CSharp result offset");
        Require(module.Functions.SelectMany(function => function.Parameters).Any(parameter => parameter.TypeId.StartsWith("type:$borrow:", StringComparison.Ordinal))
            && module.Functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions).Any(instruction => instruction.Op == "borrow_managed"),
            "CSharp parameters and captures use borrowed descriptors");
        WasmCompilationResult wasm = WasmModuleCompiler.Compile(module);
        Require(wasm.Succeeded, string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
        GuestModule stress = module with { Functions = module.Functions.Select(function => function with
        { Blocks = function.Blocks.Select(block => block with { Instructions = block.Instructions.SelectMany(instruction => instruction.Op == "managed_new"
            ? new[] { instruction, new GuestInstruction("managed_collect", null, Array.Empty<string>(), null, null, null) } : new[] { instruction }).ToArray() }).ToArray() }).ToArray() };
        WasmCompilationResult stressed = WasmModuleCompiler.Compile(stress);
        Require(stressed.Succeeded && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "borrowed CSharp stress and deterministic compilation");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
            File.WriteAllBytes(Path.Combine(directory, "csharp-borrowed.wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-borrowed-stress.wasm"), stressed.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "csharp-borrowed.guest-ir.json"), GuestIrSerializer.Serialize(module));
        }
        foreach (string call in new[] { "Host(ref value)", "Forward(ref value)" })
        {
            string rejected = "using System; using System.Runtime.InteropServices; public static class Script { "
                + "[DllImport(\"env\")] static extern void Host(ref int value); static void Forward(ref int value) { Host(ref value); } "
                + "public static int Run() { int value = 1; Func<int> read = () => value; " + call + "; return read(); } }";
            CSharpGuestLoweringResult boundary = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(rejected), new string('c', 64));
            Require(!boundary.Succeeded && boundary.Diagnostics.Any(item => item.Code == "ASCG1024"),
                "captured or forwarded Guest borrow cannot cross raw Host ref ABI");
        }
        return 8;
    }

    internal static int Reference(string source, Type? expectedFailure = null)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        CSharpCompilation compilation = CSharpCompilation.Create("BorrowedReferenceOracle", new[] { CSharpSyntaxTree.ParseText(source) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Release));
        using MemoryStream bytes = new();
        var result = compilation.Emit(bytes);
        Require(result.Success, string.Join(" | ", result.Diagnostics));
        bytes.Position = 0;
        AssemblyLoadContext context = new("borrowed-reference-oracle", isCollectible: true);
        try
        {
            Type script = context.LoadFromStream(bytes).GetType("Script")!;
            try
            {
                int value = (int)script.GetMethod("Run")!.Invoke(null, null)!;
                Require(expectedFailure is null, "reference execution was expected to fail");
                return value;
            }
            catch (System.Reflection.TargetInvocationException exception) when (expectedFailure is not null && exception.InnerException?.GetType() == expectedFailure)
            { return (int)script.GetField("Result")!.GetValue(null)!; }
        }
        finally { context.Unload(); }
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}

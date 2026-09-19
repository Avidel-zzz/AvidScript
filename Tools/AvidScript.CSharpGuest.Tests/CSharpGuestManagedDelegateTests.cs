using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestManagedDelegateTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public delegate int Update(ref int value, out int observed);
            public delegate int Combine(int first = 2, int second = 3);
            public static class Script
            {
                static int Plus(int value) => value + 10;
                static int Minus(int value) => value - 10;
                static long Plus(long value) => value + 50;
                static int Encode(int first, int second) => first * 100 + second;
                static int Next(ref int value) => value++;
                static int Counter;
                static void Add(int value) { Counter += value; }
                static int Sum(int n) { Func<int, int> callback = Sum; return n > 0 ? n + callback(n - 1) : 0; }
                static Func<int, int> Choose(int which) { if (which > 0) return Plus; return Minus; }
                static int Apply(Func<int, int> callback, int value) => callback(value);
                static int Mutate(ref int value, out int observed) { value += 4; observed = value * 2; return value + 3; }
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run(int n, int which)
                {
                    Func<int, int> callback = Choose(which);
                    int sum = 0;
                    for (int i = 0; i < n; ++i) sum += Apply(callback, i);
                    callback = Plus;
                    return sum + callback(n);
                }
                [UnmanagedCallersOnly(EntryPoint = "byref")]
                public static int ByRef(int n)
                {
                    Update callback = Mutate;
                    int result = callback(ref n, out int observed);
                    return result + n * 100 + observed * 1000;
                }
                [UnmanagedCallersOnly(EntryPoint = "local")]
                public static int Local(int n)
                {
                    static int Twice(int value) => value * 2;
                    Func<int, int> callback = Twice;
                    return callback(n);
                }
                [UnmanagedCallersOnly(EntryPoint = "named")]
                public static int Named(int n)
                {
                    Combine callback = Encode;
                    return callback(second: Next(ref n), first: Next(ref n)) + callback();
                }
                [UnmanagedCallersOnly(EntryPoint = "wide")]
                public static long Wide(long n) { Func<long, long> callback = Plus; return callback(n); }
                [UnmanagedCallersOnly(EntryPoint = "nullable")]
                public static int Nullable(int n)
                {
                    Func<int, int> callback = null;
                    if (n > 0) callback = Plus;
                    if (callback != null) return callback(n);
                    return -7;
                }
                [UnmanagedCallersOnly(EntryPoint = "action")]
                public static int Action(int n) { Counter = 3; Action<int> callback = Add; callback(n); return Counter; }
                [UnmanagedCallersOnly(EntryPoint = "recursive")]
                public static int Recursive(int n) => Sum(n);
            }
            """;
        SemanticDocument document = CSharpGuestLexicalCaptureTests.Analyze(source);
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, new string('f', 64));
        Require(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
        GuestModule module = result.Module!;
        Require(module.FunctionReferences.Count == 5
            && module.FunctionReferences.SelectMany(reference => reference.TargetFunctionIds).Distinct().Count() == 8
            && module.Functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions).Any(instruction => instruction.Op == "call_indirect"),
            "method-group targets must survive reachability and produce typed indirect calls");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Require(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Require(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(CSharpGuestLowerer.Lower(document, new string('f', 64)).Module!).Bytes),
            "source delegate compilation must be deterministic");
        Require(WasmModuleCompiler.Compile(module, new(true, 4)).CooperativeSafepointAttestation is { RecursiveFunctionCount: 1, Verified: true },
            "source delegate recursion must retain cooperative cancellation coverage");
        CSharpGuestLoweringResult debug = CSharpGuestLowerer.Lower(document, new string('f', 64), enableDebugInstrumentation: true);
        Require(debug.Succeeded && WasmModuleCompiler.Compile(debug.Module!).Succeeded,
            "debug instrumentation must retain indirect target contracts");
        Require(!CSharpGuestLowerer.Lower(document with { SchemaVersion = 21, SemanticVersion = "1.23" }, new string('f', 64)).Succeeded,
            "old semantic versions must not smuggle new delegate operations");
        CSharpGuestLoweringResult previous = CSharpGuestLowerer.Lower(document with { SchemaVersion = 21, SemanticVersion = "1.24" }, new string('f', 64));
        Require(previous.Succeeded && WasmModuleCompiler.Compile(previous.Module!).Succeeded,
            "semantic 1.24 static method groups must remain executable");
        foreach (bool wrongType in new[] { false, true })
        {
            SemanticOperation Rewrite(SemanticOperation operation)
            {
                SemanticOperation rewritten = operation with { Children = operation.Children.Select(Rewrite).ToArray() };
                if (rewritten.Kind != "invocation" || rewritten.SymbolId?.StartsWith("symbol:delegate_invoke:", StringComparison.Ordinal) != true
                    || rewritten.Children.Count < 2) return rewritten;
                SemanticOperation argument = rewritten.Children[1];
                SemanticOperation changed = wrongType
                    ? argument with { Children = new[] { argument.Children[0] with { TypeId = "type:int64" } } }
                    : argument with { SymbolId = "forged_parameter" };
                return rewritten with { Children = rewritten.Children.Select((child, index) => index == 1 ? changed : child).ToArray() };
            }
            SemanticDocument malformed = document with
            {
                ControlFlowGraphs = document.ControlFlowGraphs.Select(graph => graph with
                {
                    Blocks = graph.Blocks.Select(block => block with { Operations = block.Operations.Select(Rewrite).ToArray(),
                        BranchValue = block.BranchValue is null ? null : Rewrite(block.BranchValue) }).ToArray(),
                }).ToArray(),
            };
            CSharpGuestLoweringResult rejected = CSharpGuestLowerer.Lower(malformed, new string('f', 64));
            Require(!rejected.Succeeded && rejected.Diagnostics.Any(item => item.Code == "ASCG1024"),
                "forged delegate parameter identities and bound value types must fail closed");
        }
        SemanticDocument stored = CSharpGuestLexicalCaptureTests.Analyze("""
            using System;
            using System.Runtime.InteropServices;
            public struct Holder { public Func<int, int> Callback; }
            public static class Script {
                static Holder Stored;
                [UnmanagedCallersOnly(EntryPoint = "run")] public static int Run(int n) => n;
            }
            """);
        Require(!CSharpGuestLowerer.Lower(stored, new string('f', 64)).Succeeded,
            "module-local function references must not escape through nested persistent state");
        CSharpGuestLoweringResult captured = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(
            "using System; using System.Runtime.InteropServices; public static class Script { [UnmanagedCallersOnly(EntryPoint=\"run\")] public static int Run(int n) { int Read() => n; Func<int> callback = Read; return callback(); } }"), new string('f', 64));
        Require(captured.Succeeded && WasmModuleCompiler.Compile(captured.Module!).Succeeded,
            "capturing method groups compile with traced shared environments: " + string.Join(" | ", captured.Diagnostics.Select(item => item.Message)));
        foreach ((bool Supported, string Source) test in new[]
        {
            (true, "using System; using System.Runtime.InteropServices; public struct Counter { public int Value; public int Read() => Value; } public static class Script { [UnmanagedCallersOnly(EntryPoint=\"run\")] public static int Run(int n) { Counter value = new Counter(); value.Value = n; Func<int> callback = value.Read; return callback(); } }"),
            (false, "using System; using System.Runtime.InteropServices; public static class Script { [DllImport(\"env\", EntryPoint=\"sink\")] public static extern void Sink(Func<int,int> callback); static int Identity(int n) => n; [UnmanagedCallersOnly(EntryPoint=\"run\")] public static int Run(int n) { Sink(Identity); return n; } }"),
        })
        {
            CSharpGuestLoweringResult caseResult = CSharpGuestLowerer.Lower(CSharpGuestLexicalCaptureTests.Analyze(test.Source), new string('f', 64));
            Require(test.Supported ? caseResult.Succeeded && WasmModuleCompiler.Compile(caseResult.Module!).Succeeded
                    : !caseResult.Succeeded && caseResult.Diagnostics.Any(item => item.Code is "ASCG1024" or "ASCG1003"),
                "bound struct receivers compile while unsupported Host delegate crossings remain rejected");
        }
        string? root = Environment.GetEnvironmentVariable("AVIDSCRIPT_LOCAL_FUNCTION_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(root))
        {
            Directory.CreateDirectory(root);
            File.WriteAllBytes(Path.Combine(root, "managed-delegates.wasm"), compiled.Bytes);
        }
        return 14;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

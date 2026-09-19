using System;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestLambdaTests
{
    public static int Run()
    {
        const string source = """
            using System;
            using System.Runtime.InteropServices;
            public delegate int Update(ref int value, out int observed);
            public static class Script
            {
                static int Sum(int n) { Func<int, int> next = value => Sum(value); return n > 0 ? n + next(n - 1) : 0; }
                static Func<int, int> Callback => value => value + 17;
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run(int n)
                {
                    Func<int, int> square = static value => value * value;
                    Func<int, int> branch = value => { if (value > 0) return value + 2; return 7; };
                    Func<int, int> anonymous = delegate(int value) { int Twice() => value * 2; return Twice() + 1; };
                    Func<Func<int, int>> factory = () => item => item - 3;
                    Func<int, int> chosen = n > 0 ? (value => value + 11) : (value => value - 11);
                    return square(n) + branch(n) + anonymous(n) + factory()(n) + chosen(n);
                }
                [UnmanagedCallersOnly(EntryPoint = "local")]
                public static int Local(int n)
                {
                    static Func<int, int> Make() => static value => value * 4;
                    const int offset = 5;
                    Func<int> constant = () => offset;
                    Func<int, int> ignored = delegate { return 3; };
                    return Make()(n) + constant() + ignored(n);
                }
                [UnmanagedCallersOnly(EntryPoint = "byref")]
                public static int ByRef(int n)
                {
                    Update callback = (ref int value, out int observed) => { value += 2; observed = value * 3; return value + 1; };
                    int result = callback(ref n, out int observed);
                    return n * 1000 + observed * 10 + result;
                }
                [UnmanagedCallersOnly(EntryPoint = "recursive")]
                public static int Recursive(int n) => Sum(n);
                [UnmanagedCallersOnly(EntryPoint = "property")]
                public static int Property(int n) => Callback(n);
            }
            """;
        SemanticDocument document = CSharpGuestLexicalCaptureTests.Analyze(source);
        SemanticCallable[] lambdas = document.Callables.Where(item => item.MethodSymbolId.Contains(":lambda:", StringComparison.Ordinal)
            && !item.MethodSymbolId.Contains(":local:", StringComparison.Ordinal)).ToArray();
        Require(lambdas.Length >= 8 && lambdas.All(item => item.IsStatic && item.HasBody)
            && lambdas.All(item => document.ControlFlowGraphs.Any(graph => graph.MethodSymbolId == item.MethodSymbolId)),
            "lambda declarations need independent static callables and CFGs");
        SemanticDocument spaced = CSharpGuestLexicalCaptureTests.Analyze("\n\n" + source);
        Require(document.Callables.Select(item => item.MethodSymbolId).SequenceEqual(spaced.Callables.Select(item => item.MethodSymbolId)),
            "lambda and nested local identities must survive whitespace changes");
        CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(document, new string('b', 64));
        Require(result.Succeeded, string.Join(" | ", result.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(result.Module!);
        Require(compiled.Succeeded && compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(result.Module!).Bytes),
            "lambda lowering must produce deterministic executable WASM");
        Require(WasmModuleCompiler.Compile(result.Module!, new(true, 4)).CooperativeSafepointAttestation is { RecursiveFunctionCount: 2, Verified: true },
            "lambda-mediated recursion must participate in cooperative polling");
        Require(!CSharpGuestLowerer.Lower(document with { SchemaVersion = 21, SemanticVersion = "1.24" }, new string('b', 64)).Succeeded,
            "lambda callables must not execute under an older semantic version");
        CSharpGuestLoweringResult previous = CSharpGuestLowerer.Lower(document with { SchemaVersion = 21, SemanticVersion = "1.25" }, new string('b', 64));
        Require(previous.Succeeded && WasmModuleCompiler.Compile(previous.Module!).Succeeded,
            "schema 21/1.25 noncapturing lambdas must remain executable");
        CSharpGuestLoweringResult previousClosureSchema = CSharpGuestLowerer.Lower(document with { SchemaVersion = 22, SemanticVersion = "1.26" }, new string('b', 64));
        Require(previousClosureSchema.Succeeded && WasmModuleCompiler.Compile(previousClosureSchema.Module!).Succeeded,
            "schema 22/1.26 noncapturing lambdas must remain executable");
        CSharpGuestLoweringResult debug = CSharpGuestLowerer.Lower(document, new string('b', 64), enableDebugInstrumentation: true);
        Require(debug.Succeeded && WasmModuleCompiler.Compile(debug.Module!).Succeeded,
            "lambda functions must retain a valid signature under debug instrumentation");
        foreach (string body in new[]
        {
            "Func<int> callback = () => n; return callback();",
            "int Read() => n; Func<int> callback = () => Read(); return callback();",
            "Func<Func<int>> callback = () => () => n; return callback()();",
            "Func<int> callback = () => { int local = n; return local; }; return callback();",
        })
        {
            string captured = "using System; public static class Script { public static int Run(int n) { " + body + " } }";
            var frontend = AvidScript.CSharpFrontend.FrontendAnalyzer.Analyze(captured, "Scripts/CapturedLambda.cs");
            SemanticDocument rejected = SemanticAnalyzer.Analyze(captured, "Scripts/CapturedLambda.cs", frontend.Source.Sha256);
            Require(rejected.Succeeded && rejected.ControlFlowGraphs.Count != 0
                && rejected.ClosureEnvironments.All(environment => environment.Allocation is not null)
                && CSharpGuestLowerer.Lower(rejected, new string('b', 64)).Diagnostics.Any(item => item.Code == "ASCG1024"),
                "direct, transitive and nested captures need allocation plans and a Guest execution guard until lowering is connected");
        }
        foreach (string captured in new[]
        {
            "using System; public class Script { int value; public int Run() { Func<int> callback = () => value; return callback(); } }",
            "using System; public class Script { int field; public int Value { set { Func<int> callback = () => value; field = callback(); } } }",
        })
        {
            var frontend = AvidScript.CSharpFrontend.FrontendAnalyzer.Analyze(captured, "Scripts/ReceiverLambda.cs");
            SemanticDocument rejected = SemanticAnalyzer.Analyze(captured, "Scripts/ReceiverLambda.cs", frontend.Source.Sha256);
            Require(rejected.Succeeded && rejected.ClosureEnvironments.All(environment => environment.Allocation is not null)
                && CSharpGuestLowerer.Lower(rejected, new string('b', 64)).Diagnostics.Any(item => item.Code == "ASCG1024"),
                "receiver and implicit setter captures need complete plans before Guest execution");
        }
        foreach (string unsupported in new[]
        {
            "using System; using System.Threading.Tasks; public static class Script { public static int Run() { Func<Task<int>> callback = async () => 1; return 0; } }",
            "using System; public static class Script { static Func<int> callback = () => 1; public static int Run() => callback(); }",
        })
        {
            var frontend = AvidScript.CSharpFrontend.FrontendAnalyzer.Analyze(unsupported, "Scripts/UnsupportedLambda.cs");
            SemanticDocument rejected = SemanticAnalyzer.Analyze(unsupported, "Scripts/UnsupportedLambda.cs", frontend.Source.Sha256);
            Require(!rejected.Succeeded && rejected.ControlFlowGraphs.Count == 0
                && rejected.Diagnostics.Any(item => item.Code == "ASCS4001"),
                "async lambdas and field initializers must remain explicitly unsupported");
        }
        string? root = Environment.GetEnvironmentVariable("AVIDSCRIPT_LOCAL_FUNCTION_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(root))
        {
            Directory.CreateDirectory(root);
            File.WriteAllBytes(Path.Combine(root, "lambdas.wasm"), compiled.Bytes);
        }
        return 17;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

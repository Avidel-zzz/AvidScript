using System;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticLocalFunctionTests
{
    public static int Run()
    {
        const string source = """
            using System.Runtime.InteropServices;
            public static class Script
            {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run(int value)
                {
                    static int Calculate(int n)
                    {
                        static int Double(int x) => x * 2;
                        return n <= 1 ? Double(n) : Calculate(n - 1) + Double(n);
                    }
                    return Calculate(value);
                }
                public static int Other(int value)
                {
                    static int Calculate(int n) => n + 100;
                    return Calculate(value);
                }
            }
            """;
        SemanticDocument document = Analyze(source);
        Require(document.Succeeded, Describe(document));
        Require(document.Callables.Count == 5 && document.ControlFlowGraphs.Count == 5,
            "each lexical function must own exactly one callable and CFG");
        Require(document.Callables.Select(item => item.MethodSymbolId).Distinct().Count() == 5,
            "same-named local functions must not collide across owners");
        Require(document.Reachability!.ReachableCallableIds.Count == 3,
            "reachable closure must include recursive/nested functions but exclude unused owner");
        SemanticDocument repeated = Analyze(source);
        Require(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(repeated)),
            "local function projection must be byte deterministic");

        const string siblingSource = """
            public static class Script
            {
                public static int Run(bool first)
                {
                    if (first) { static int Value() => 11; return Value(); }
                    else { static int Value() => 29; return Value(); }
                }
            }
            """;
        SemanticDocument siblings = Analyze(siblingSource);
        Require(siblings.Succeeded && siblings.Callables.Count == 3, Describe(siblings));
        SemanticDocument spaced = Analyze("\n\n" + siblingSource);
        Require(siblings.Callables.Select(item => item.MethodSymbolId)
            .SequenceEqual(spaced.Callables.Select(item => item.MethodSymbolId)),
            "lexical function identities must survive whitespace changes");

        foreach (string declaration in new[]
        {
            "static System.Func<int> Local() => () => 1;",
            "static T Local<T>(T x) => x;",
            "static async System.Threading.Tasks.Task Local() { await System.Threading.Tasks.Task.Delay(1); }",
            "static extern int Local();",
        })
        {
            SemanticDocument rejected = Analyze(
                "public static class Script { public static int Run(int value) { " + declaration + " return value; } }");
            Require(!rejected.Succeeded && rejected.Diagnostics.Any(item => item.Code is "ASCS4010" or "ASCS4001")
                && rejected.ControlFlowGraphs.Count == 0,
                "unsupported local function contracts must fail closed with an actionable diagnostic");
        }

        const string reference = "public static class Helper { public static int Run(int x) { static int Local(int n) => n * 3; return Local(x); } }";
        const string caller = "public static class Script { public static int Run(int x) => Helper.Run(x); }";
        FrontendDocument callerFrontend = FrontendAnalyzer.Analyze(caller, "Scripts/Caller.cs");
        SemanticDocument referenced = SemanticAnalyzer.Analyze(caller, "Scripts/Caller.cs",
            callerFrontend.Source.Sha256,
            new[] { new SemanticReferenceSource(reference, "Scripts/Helper.cs", true) });
        Require(referenced.Succeeded && referenced.ControlFlowGraphs.Count == 3, Describe(referenced));

        const string exportSource = """
            namespace AvidScript
            {
                [System.AttributeUsage(System.AttributeTargets.Method)]
                public sealed class AvidExportAttribute : System.Attribute
                {
                    public AvidExportAttribute(string name) { }
                }
            }
            public static class Script
            {
                public static int Run()
                {
                    [AvidScript.AvidExport("local")]
                    static int Local() => 1;
                    return Local();
                }
            }
            """;
        SemanticDocument exported = Analyze(exportSource);
        Require(!exported.Succeeded && exported.Diagnostics.Any(item => item.Code == "ASCS5010"),
            "lexical functions must not accidentally become public WASM exports");

        const string receiverSource = """
            public class Counter
            {
                public int Value;
                public int Run(int delta)
                {
                    void Add() { Value += delta; }
                    int Read() => Value;
                    Add();
                    return Read();
                }
            }
            """;
        SemanticDocument receiver = Analyze(receiverSource);
        Require(receiver.Succeeded, Describe(receiver));
        SemanticCallable add = receiver.Callables.Single(item => item.MethodSymbolId.Contains(":local:Add("));
        Require(add.IsStatic && add.Parameters.Count == 2
            && add.Parameters.Single(item => item.Name == "this").RefKind == "none"
            && add.Parameters.Single(item => item.Name == "delta").RefKind == "ref",
            "receiver identity and mutable parameter capture must have distinct explicit passing contracts");

        foreach (string invalidSource in new[]
        {
            "public class Script { public int Run(ref int x) { int Read() => x; return Read(); } }",
            "public struct Script { public int Value; public int Run() { int Read() => Value; return Read(); } }",
            "public class Script { public int Run() { int x; int Read() => x; return Read(); } }",
        })
        {
            SemanticDocument invalid = Analyze(invalidSource);
            Require(!invalid.Succeeded && invalid.Diagnostics.Any(item => item.Severity == "error" && item.Code.StartsWith("CS")),
                "Roslyn illegal ref/struct/unassigned capture diagnostics must remain authoritative");
        }
        return 13;
    }

    private static SemanticDocument Analyze(string source)
    {
        const string sourceId = "Scripts/LocalFunctions.cs";
        FrontendDocument frontend = FrontendAnalyzer.Analyze(source, sourceId);
        return SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256);
    }

    private static string Describe(SemanticDocument document) => string.Join(" | ",
        document.Diagnostics.Select(item => $"{item.Code}: {item.Message}"));

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

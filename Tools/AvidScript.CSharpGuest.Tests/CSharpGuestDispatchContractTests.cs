using System;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.WasmBackend;

internal static class CSharpGuestDispatchContractTests
{
    public static int Run()
    {
        SemanticDocument document = CSharpGuestLexicalCaptureTests.Analyze("""
            using System;
            using System.Runtime.InteropServices;
            public class Counter { public int Value; public int Read() => Value; }
            public static class Script {
                [UnmanagedCallersOnly(EntryPoint = "run")]
                public static int Run() { Counter c = new Counter(); c.Value = 7; Func<int> read = c.Read; return read(); }
            }
            """);
        int count = 0;
        var current = CSharpGuestLowerer.Lower(document, new string('a', 64));
        Check(current.Succeeded && WasmModuleCompiler.Compile(current.Module!).Succeeded, "current direct instance delegates still compile to WASM");
        SemanticDocument legacy = CSharpGuestSemanticFixture.WithoutDispatch(document) with { SchemaVersion = 24, SemanticVersion = "1.28" };
        var previous = CSharpGuestLowerer.Lower(legacy, new string('a', 64));
        Check(previous.Succeeded && WasmModuleCompiler.Compile(previous.Module!).Succeeded, "schema 24 reference objects and bound delegates remain executable: "
            + string.Join(" | ", previous.Diagnostics.Select(item => item.Code + ": " + item.Message)));
        SemanticCallable target = document.Callables.Single(item => item.MethodSymbolId.Contains("Counter.Read(", StringComparison.Ordinal));
        foreach (SemanticCallableDispatch? dispatch in new SemanticCallableDispatch?[]
        {
            null, target.Dispatch! with { IsOverride = true }, target.Dispatch! with { SlotMethodSymbolId = "forged" },
            target.Dispatch! with { IsSealed = true }, target.Dispatch! with { ExplicitInterfaceMethodIds = null! },
        }) Reject(document with { Callables = document.Callables.Select(item => item == target ? item with { Dispatch = dispatch } : item).ToArray() });
        Reject(document with { SchemaVersion = 24, SemanticVersion = "1.28" });
        Reject(CSharpGuestSemanticFixture.WithoutDispatch(document));
        foreach (SemanticMethodDispatch? dispatch in new SemanticMethodDispatch?[]
        {
            null, new("unknown", null, false), new("static", null, false),
            new("virtual", null, false), new("interface", null, false), new("virtual", "forged", true),
        })
        {
            SemanticOperation Rewrite(SemanticOperation operation) => operation with {
                Dispatch = operation.Kind == "method_reference" && operation.SymbolId == target.MethodSymbolId ? dispatch : operation.Dispatch,
                Children = operation.Children.Select(Rewrite).ToArray() };
            // The executable CFG is independently validated, not just the source operation tree.
            Reject(document with { ControlFlowGraphs = document.ControlFlowGraphs.Select(graph => graph with {
                Blocks = graph.Blocks.Select(block => block with {
                    Operations = block.Operations.Select(Rewrite).ToArray(),
                    BranchValue = block.BranchValue is null ? null : Rewrite(block.BranchValue) }).ToArray() }).ToArray() });
        }
        return count;

        void Reject(SemanticDocument malformed)
        {
            CSharpGuestLoweringResult result = CSharpGuestLowerer.Lower(malformed, new string('a', 64));
            Check(!result.Succeeded && result.Module is null && result.Diagnostics.Any(item => item.Code == "ASCG1001"),
                "malformed/downgraded dispatch must be rejected before executable planning");
        }
        void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); count++; }
    }
}

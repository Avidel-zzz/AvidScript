using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpSemantic;

internal static class SemanticClosureAllocationTests
{
    public static int Run()
    {
        const string source = """
            using System;
            public static class Script {
                public static int Run(int seed) {
                    Func<int> outer = () => seed;
                    int sum = 0;
                    for (int i = 0; i < 5; i++) {
                        int copy = i;
                        Func<int> read = () => i + copy;
                        if (i == 1) continue;
                        if (i == 3) break;
                        sum += copy;
                    }
                    return sum;
                }
            }
            """;
        SemanticDocument document = Analyze(source);
        Require(document.Succeeded, string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Require(SemanticClosureContractValidator.IsValid(document), "allocation contract");
        SemanticControlFlowGraph graph = document.ControlFlowGraphs.Single(item => item.MethodSymbolId.Contains("Script.Run(", StringComparison.Ordinal)
            && !item.MethodSymbolId.Contains(":lambda:", StringComparison.Ordinal));
        Dictionary<string, int> allocations = TraceIntegerControlFlow(document, graph);
        Require(allocations["activation"] == 1 && allocations["for_entry"] == 1 && allocations["block_entry"] == 4,
            "activation and for cell allocate once; body cell allocates on four entries including continue/break iterations");
        SemanticClosureEnvironment loop = document.ClosureEnvironments.Single(item => item.ScopeKind == "for_entry");
        SemanticClosureAllocation plan = loop.Allocation!;
        Require(graph.Blocks.SelectMany(block => block.Successors).Any(edge => edge.DestinationBlockOrdinal <= edge.SourceBlockOrdinal
            && Inside(plan, edge.SourceBlockOrdinal) && Inside(plan, edge.DestinationBlockOrdinal)
            && !plan.Entries.Contains(new(edge.SourceBlockOrdinal, edge.DestinationBlockOrdinal))), "for backedge must not reallocate its captured initializer");

        SemanticDocument each = Analyze("using System; public static class Script { public static Func<int> Make(int[] values) { Func<int> result = null; foreach (int item in values) { result = () => item; } return result; } }");
        Require(!each.Succeeded && each.ControlFlowGraphs.Count == 0 && each.Diagnostics.Any(item => item.Code == "ASCS3001"),
            "foreach enumerator exception flow must remain non-executable until its lowering is implemented");
        SemanticClosureEnvironment iteration = each.ClosureEnvironments.Single();
        Require(iteration.ScopeKind == "foreach_iteration" && iteration.Allocation!.Entries.Count == 1
            && !Inside(iteration.Allocation, iteration.Allocation.Entries[0].SourceBlockOrdinal!.Value),
            "foreach analysis retains an iteration lifetime entry without claiming an executable CFG");

        SemanticDocument siblings = Analyze("using System; public static class Script { public static Func<int> Make(bool flag) { if (flag) { int x = 1; return () => x; } else { int x = 2; return () => x; } } }");
        Require(siblings.Succeeded && SemanticClosureContractValidator.IsValid(siblings)
            && siblings.ClosureEnvironments.Select(item => item.Allocation!.FirstBlockOrdinal).Distinct().Count() == 2,
            "sibling lexical scopes retain distinct CFG allocation regions");
        Require(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document)))), "allocation canonical round trip");
        Require(SemanticSerializer.Serialize(document).SequenceEqual(SemanticSerializer.Serialize(Analyze(source))), "allocation projection determinism");

        SemanticClosureAllocation[] malformed = {
            plan with { FirstBlockOrdinal = -1 }, plan with { LastBlockOrdinal = int.MaxValue },
            plan with { Entries = null! }, plan with { Entries = new SemanticClosureEntry[] { null! } },
            plan with { Entries = Array.Empty<SemanticClosureEntry>() },
            plan with { Entries = plan.Entries.Concat(plan.Entries).ToArray() },
            plan with { Entries = new[] { new SemanticClosureEntry(null, plan.FirstBlockOrdinal) } },
            plan with { Entries = new[] { new SemanticClosureEntry(plan.LastBlockOrdinal, plan.FirstBlockOrdinal) } },
        };
        foreach (SemanticClosureAllocation invalid in malformed)
            Require(!SemanticClosureContractValidator.IsValid(Replace(document, loop, loop with { Allocation = invalid })), "invalid allocation must fail closed");
        Require(!SemanticClosureContractValidator.IsValid(Replace(document, loop, loop with { Allocation = null })), "missing allocation must fail closed");
        Require(!SemanticClosureContractValidator.IsValid(document with { SchemaVersion = 22, SemanticVersion = "1.26" }), "allocation metadata cannot be smuggled into a legacy contract");
        Require(SemanticClosureContractValidator.IsValid(document with { SchemaVersion = 22, SemanticVersion = "1.26",
            ClosureEnvironments = document.ClosureEnvironments.Select(item => item with { Allocation = null }).ToArray() }), "legacy environment analysis remains readable without claiming allocation support");
        Require(!SemanticClosureContractValidator.IsValid(document with { SchemaVersion = 24, SemanticVersion = "1.28" }), "future allocation contract must fail closed");
        Require(!SemanticClosureContractValidator.IsValid(Replace(document, loop, loop with { Allocation = plan with { Entries = null! } })
            with { Succeeded = false, ControlFlowGraphs = Array.Empty<SemanticControlFlowGraph>() }),
            "failed analysis still validates allocation metadata shape even when executable graphs are withheld");
        return 22;
    }

    // Execute only the integer CFG of this fixture; delegate expressions are inert.
    // This verifies allocation timing against control flow, not closure execution.
    private static Dictionary<string, int> TraceIntegerControlFlow(SemanticDocument document, SemanticControlFlowGraph graph)
    {
        Dictionary<string, int> values = new(StringComparer.Ordinal), counts = new(StringComparer.Ordinal);
        SemanticClosureEnvironment[] environments = document.ClosureEnvironments.Where(item => item.OwnerMethodSymbolId == graph.MethodSymbolId).ToArray();
        int? previous = null; int current = graph.EntryBlockOrdinal;
        for (int steps = 0; steps < 100; ++steps)
        {
            foreach (SemanticClosureEnvironment environment in environments)
                if (environment.Allocation!.Entries.Contains(new(previous, current))) counts[environment.ScopeKind] = counts.GetValueOrDefault(environment.ScopeKind) + 1;
            SemanticBasicBlock block = graph.Blocks.Single(item => item.Ordinal == current);
            foreach (SemanticOperation operation in block.Operations) Eval(operation);
            if (block.Successors.Count == 0 || block.Successors.Any(edge => edge.Semantics == "return")) return counts;
            SemanticControlFlowEdge next;
            if (block.BranchValue is null) next = block.Successors.Single();
            else
            {
                bool condition = Eval(block.BranchValue) != 0;
                bool conditional = block.ConditionKind == "when_true" ? condition : !condition;
                next = block.Successors.Single(edge => edge.Kind == (conditional ? "conditional" : "fallthrough"));
            }
            previous = current; current = next.DestinationBlockOrdinal;
        }
        throw new InvalidOperationException("integer CFG trace exceeded bounded steps");

        int Eval(SemanticOperation operation)
        {
            switch (operation.Kind)
            {
                case "expression_statement": case "conversion": return Eval(operation.Children[0]);
                case "literal": return int.Parse(operation.Constant!.Value!, System.Globalization.CultureInfo.InvariantCulture);
                case "delegate_creation": return 0;
                case "local_reference": case "parameter_reference": return values.GetValueOrDefault(operation.SymbolId!);
                case "assignment": return values[operation.Children[0].SymbolId!] = Eval(operation.Children[1]);
                case "compound_assignment": return values[operation.Children[0].SymbolId!] += Eval(operation.Children[1]);
                case "increment_or_decrement": return ++values[operation.Children[0].SymbolId!];
                case "binary":
                    int left = Eval(operation.Children[0]), right = Eval(operation.Children[1]);
                    return operation.OperatorKind switch { "less_than" => left < right ? 1 : 0, "equals" => left == right ? 1 : 0,
                        "add" => left + right, _ => throw new InvalidOperationException("unexpected integer operator " + operation.OperatorKind) };
                default: throw new InvalidOperationException("unexpected integer CFG operation " + operation.Kind);
            }
        }
    }
    private static bool Inside(SemanticClosureAllocation plan, int block) => block >= plan.FirstBlockOrdinal && block <= plan.LastBlockOrdinal;
    private static SemanticDocument Replace(SemanticDocument document, SemanticClosureEnvironment original, SemanticClosureEnvironment replacement)
        => document with { ClosureEnvironments = document.ClosureEnvironments.Select(item => item == original ? replacement : item).ToArray() };
    private static SemanticDocument Analyze(string source)
    { var frontend = FrontendAnalyzer.Analyze(source, "Scripts/ClosureAllocation.cs"); return SemanticAnalyzer.Analyze(source, "Scripts/ClosureAllocation.cs", frontend.Source.Sha256); }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}

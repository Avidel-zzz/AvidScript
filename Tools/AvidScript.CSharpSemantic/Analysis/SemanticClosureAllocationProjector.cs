using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis.FlowAnalysis;

namespace AvidScript.CSharpSemantic;

internal static class SemanticClosureAllocationProjector
{
    public static IReadOnlyList<SemanticClosureEnvironment> Project(SemanticCompilationContext context,
        IReadOnlyDictionary<string, SemanticExecutableBody> owners, IReadOnlyList<SemanticClosureEnvironment> environments,
        List<SemanticDiagnostic> diagnostics)
    {
        Dictionary<string, ControlFlowGraph> graphs = new(StringComparer.Ordinal);
        List<SemanticClosureEnvironment> result = new();
        foreach (SemanticClosureEnvironment environment in environments)
        {
            SemanticExecutableBody owner = owners[environment.OwnerMethodSymbolId];
            // Async uses a separate resumable CFG. Its allocation points require the
            // continuation contract; never infer them from a synchronous Roslyn graph.
            if (owner.Method.IsAsync) { result.Add(environment); continue; }
            try
            {
                if (!graphs.TryGetValue(environment.OwnerMethodSymbolId, out ControlFlowGraph? graph))
                {
                    graph = SemanticControlFlowProjector.CreateGraph(owner, context.Compilation.GetSemanticModel(owner.Unit.SyntaxTree));
                    graphs.Add(environment.OwnerMethodSymbolId, graph);
                }
                int first = 0, last = graph.Blocks.Length - 1;
                SemanticClosureEntry[] entries;
                if (environment.ScopeKind == "activation") entries = new[] { new SemanticClosureEntry(null, first) };
                else
                {
                    string[] cells = environment.Cells.Select(cell => cell.SymbolId).ToArray();
                    ControlFlowRegion[] matching = Regions(graph.Root).Where(region =>
                        cells.All(cell => region.Locals.Any(local => SemanticSymbolProjector.GetSymbolId(local) == cell))).ToArray();
                    if (matching.Length != 1) throw new InvalidOperationException("Captured cells need one exact Roslyn local lifetime region.");
                    first = matching[0].FirstBlockOrdinal; last = matching[0].LastBlockOrdinal;
                    entries = graph.Blocks.Where(block => block.Ordinal >= first && block.Ordinal <= last)
                        .SelectMany(block => block.Predecessors)
                        .Where(edge => edge.Source.Ordinal < first || edge.Source.Ordinal > last)
                        .Select(edge => new SemanticClosureEntry(edge.Source.Ordinal, edge.Destination!.Ordinal))
                        .Distinct().OrderBy(edge => edge.SourceBlockOrdinal).ThenBy(edge => edge.DestinationBlockOrdinal).ToArray();
                }
                result.Add(environment with { Allocation = new(first, last, entries) });
            }
            catch (Exception exception) when (exception is ArgumentException or InvalidOperationException)
            {
                diagnostics.Add(new("ASCS4004", "error", $"Closure scope '{environment.Id}' has no exact control-flow allocation plan: {exception.Message}", environment.Span));
            }
        }
        return result;
    }

    private static IEnumerable<ControlFlowRegion> Regions(ControlFlowRegion root)
    {
        Stack<ControlFlowRegion> pending = new(); pending.Push(root);
        while (pending.TryPop(out ControlFlowRegion? region))
        {
            yield return region;
            foreach (ControlFlowRegion nested in region.NestedRegions.Reverse()) pending.Push(nested);
        }
    }
}

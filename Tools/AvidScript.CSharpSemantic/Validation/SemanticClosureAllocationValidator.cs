using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticClosureAllocationValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.ClosureEnvironments is null) return false;
        if (document.SchemaVersion < 23)
            return document.ClosureEnvironments.All(environment => environment is not null && environment.Allocation is null);
        if (!((document.SchemaVersion == 23 && document.SemanticVersion == "1.27")
                || (document.SchemaVersion == 24 && document.SemanticVersion == "1.28")
                || (document.SchemaVersion == 25 && document.SemanticVersion == "1.29")
                || (document.SchemaVersion == 26 && document.SemanticVersion == "1.30")
                || (document.SchemaVersion == 27 && document.SemanticVersion == "1.31")
                || (document.SchemaVersion == 28 && document.SemanticVersion == "1.32")
                || (document.SchemaVersion == 29 && document.SemanticVersion == "1.33")
                || (document.SchemaVersion == SemanticContract.CurrentSchemaVersion && document.SemanticVersion == SemanticContract.CurrentSemanticVersion))
            || document.ControlFlowGraphs is null || document.AsyncMethods is null
            || document.ControlFlowGraphs.Any(graph => graph is null || graph.MethodSymbolId is null)
            || document.ControlFlowGraphs.Select(graph => graph.MethodSymbolId).Distinct(StringComparer.Ordinal).Count() != document.ControlFlowGraphs.Count)
            return false;
        Dictionary<string, SemanticControlFlowGraph> graphs = document.ControlFlowGraphs.ToDictionary(graph => graph.MethodSymbolId, StringComparer.Ordinal);
        foreach (SemanticClosureEnvironment environment in document.ClosureEnvironments)
        {
            if (environment is null || environment.OwnerMethodSymbolId is null) return false;
            if (environment.Allocation is not null && !ValidateShape(environment)) return false;
            if (document.AsyncMethods.Any(method => method is not null && method.MethodSymbolId == environment.OwnerMethodSymbolId))
            {
                if (environment.Allocation is not null) return false;
                continue;
            }
            // Failed compilation may retain analysis while executable CFGs are withheld.
            if (!document.Succeeded && document.ControlFlowGraphs.Count == 0) continue;
            if (!graphs.TryGetValue(environment.OwnerMethodSymbolId, out SemanticControlFlowGraph? graph)
                || !Validate(environment, graph)) return false;
        }
        return true;
    }

    private static bool Validate(SemanticClosureEnvironment environment, SemanticControlFlowGraph graph)
    {
        SemanticClosureAllocation? plan = environment.Allocation;
        if (plan is null || plan.Entries is null || plan.Entries.Any(edge => edge is null)
            || graph.Blocks is null || graph.Blocks.Any(block => block is null || block.Successors is null || block.Successors.Any(edge => edge is null))
            || plan.FirstBlockOrdinal < graph.EntryBlockOrdinal || plan.LastBlockOrdinal > graph.ExitBlockOrdinal
            || plan.FirstBlockOrdinal > plan.LastBlockOrdinal || plan.Entries.Distinct().Count() != plan.Entries.Count)
            return false;
        HashSet<int> ordinals = graph.Blocks.Select(block => block.Ordinal).ToHashSet();
        if (ordinals.Count != graph.Blocks.Count || !ordinals.Contains(plan.FirstBlockOrdinal) || !ordinals.Contains(plan.LastBlockOrdinal)) return false;
        SemanticClosureEntry[] expected;
        if (environment.ScopeKind == "activation")
        {
            if (plan.FirstBlockOrdinal != graph.EntryBlockOrdinal || plan.LastBlockOrdinal != graph.ExitBlockOrdinal) return false;
            expected = new[] { new SemanticClosureEntry(null, graph.EntryBlockOrdinal) };
        }
        else
        {
            if (plan.FirstBlockOrdinal <= graph.EntryBlockOrdinal || plan.LastBlockOrdinal >= graph.ExitBlockOrdinal) return false;
            bool Inside(int ordinal) => ordinal >= plan.FirstBlockOrdinal && ordinal <= plan.LastBlockOrdinal;
            expected = graph.Blocks.SelectMany(block => block.Successors)
                .Where(edge => !Inside(edge.SourceBlockOrdinal) && Inside(edge.DestinationBlockOrdinal))
                .Select(edge => new SemanticClosureEntry(edge.SourceBlockOrdinal, edge.DestinationBlockOrdinal))
                .Distinct().OrderBy(edge => edge.SourceBlockOrdinal).ThenBy(edge => edge.DestinationBlockOrdinal).ToArray();
        }
        return plan.Entries.SequenceEqual(expected);
    }

    private static bool ValidateShape(SemanticClosureEnvironment environment)
    {
        SemanticClosureAllocation plan = environment.Allocation!;
        if (plan.FirstBlockOrdinal < 0 || plan.LastBlockOrdinal < plan.FirstBlockOrdinal || plan.Entries is null
            || plan.Entries.Any(entry => entry is null || entry.DestinationBlockOrdinal < plan.FirstBlockOrdinal
                || entry.DestinationBlockOrdinal > plan.LastBlockOrdinal || entry.SourceBlockOrdinal < 0)
            || plan.Entries.Distinct().Count() != plan.Entries.Count) return false;
        if (environment.ScopeKind == "activation")
            return plan.FirstBlockOrdinal == 0 && plan.Entries.Count == 1
                && plan.Entries[0] == new SemanticClosureEntry(null, 0);
        return plan.Entries.All(entry => entry.SourceBlockOrdinal is { } source
                && (source < plan.FirstBlockOrdinal || source > plan.LastBlockOrdinal))
            && plan.Entries.SequenceEqual(plan.Entries.OrderBy(entry => entry.SourceBlockOrdinal).ThenBy(entry => entry.DestinationBlockOrdinal));
    }
}

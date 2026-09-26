using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Derived facts only. The source adapter owns statement order and lexical scope
// validation; this solver never authorizes a read from the possible-owner set.
public sealed record SemanticAsyncTaskOwnerBlock(
    int Ordinal, IReadOnlyList<string> GeneratedOwners, IReadOnlyList<int> Successors);

public readonly record struct SemanticAsyncTaskOwnerEdge(int Source, int Target);

public sealed record SemanticAsyncTaskOwnerStates(
    IReadOnlyDictionary<int, IReadOnlyList<string>> PossibleAtEntry,
    IReadOnlyDictionary<int, IReadOnlyList<string>> PossibleAtExit,
    IReadOnlyDictionary<int, IReadOnlyList<string>> DefiniteAtEntry,
    IReadOnlyDictionary<int, IReadOnlyList<string>> DefiniteAtExit,
    IReadOnlyDictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> ReleasedOnEdge);

public static class SemanticAsyncTaskOwnerFlowSolver
{
    public static bool TrySolve(int entry, IReadOnlyCollection<string> localIds,
        IReadOnlyList<SemanticAsyncTaskOwnerBlock> blocks,
        IReadOnlyDictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>> releases,
        out SemanticAsyncTaskOwnerStates? result)
    {
        result = null;
        if (localIds is null || blocks is null || releases is null || localIds.Count < 1
            || localIds.Count > SemanticAsyncInvocationValidator.MaximumTaskLocalsPerMethod
            || blocks.Count < 1 || blocks.Count > SemanticAsyncMethod.MaximumControlFlowSegments) return false;
        HashSet<string> owned = new(localIds, StringComparer.Ordinal);
        if (owned.Count != localIds.Count || owned.Any(string.IsNullOrWhiteSpace)) return false;
        Dictionary<int, SemanticAsyncTaskOwnerBlock> nodes = new();
        foreach (var block in blocks)
        {
            if (block is null || block.Ordinal < 0 || !nodes.TryAdd(block.Ordinal, block)
                || block.GeneratedOwners is null || block.Successors is null
                || block.GeneratedOwners.Distinct(StringComparer.Ordinal).Count() != block.GeneratedOwners.Count
                || block.GeneratedOwners.Any(id => id is null || !owned.Contains(id))
                || block.Successors.Distinct().Count() != block.Successors.Count) return false;
        }
        if (!nodes.ContainsKey(entry) || nodes.Values.SelectMany(node => node.Successors)
            .Any(target => !nodes.ContainsKey(target))) return false;
        foreach (var pair in releases)
        {
            if (!nodes.TryGetValue(pair.Key.Source, out var source) || !source.Successors.Contains(pair.Key.Target)
                || pair.Value is null || pair.Value.Distinct(StringComparer.Ordinal).Count() != pair.Value.Count
                || pair.Value.Any(id => id is null || !owned.Contains(id))) return false;
        }

        List<int> reachable = new();
        HashSet<int> visited = new();
        Queue<int> pending = new();
        pending.Enqueue(entry);
        while (pending.TryDequeue(out int ordinal))
        {
            if (!visited.Add(ordinal)) continue;
            reachable.Add(ordinal);
            foreach (int target in nodes[ordinal].Successors) pending.Enqueue(target);
        }
        var predecessors = reachable.ToDictionary(id => id, _ => new List<int>());
        foreach (int ordinal in reachable)
            foreach (int target in nodes[ordinal].Successors) predecessors[target].Add(ordinal);
        var possibleIn = reachable.ToDictionary(id => id, _ => new HashSet<string>(StringComparer.Ordinal));
        var possibleOut = reachable.ToDictionary(id => id, _ => new HashSet<string>(StringComparer.Ordinal));
        var definiteIn = reachable.ToDictionary(id => id, _ => new HashSet<string>(owned, StringComparer.Ordinal));
        var definiteOut = reachable.ToDictionary(id => id, _ => new HashSet<string>(owned, StringComparer.Ordinal));
        bool converged = false;
        // Edge retirement is a monotone kill; each may/must bit changes once.
        int limit = 2 * reachable.Count * (owned.Count + 1) + 1;
        for (int iteration = 0; iteration < limit; ++iteration)
        {
            bool changed = false;
            foreach (int ordinal in reachable)
            {
                HashSet<string> may = new(StringComparer.Ordinal);
                HashSet<string> must = ordinal == entry
                    ? new(StringComparer.Ordinal) : new(owned, StringComparer.Ordinal);
                foreach (int predecessor in predecessors[ordinal])
                {
                    var edge = new SemanticAsyncTaskOwnerEdge(predecessor, ordinal);
                    var killed = releases.GetValueOrDefault(edge) ?? Array.Empty<string>();
                    may.UnionWith(possibleOut[predecessor].Except(killed, StringComparer.Ordinal));
                    must.IntersectWith(definiteOut[predecessor].Except(killed, StringComparer.Ordinal));
                }
                changed |= Replace(possibleIn, ordinal, may);
                changed |= Replace(definiteIn, ordinal, must);
                may = new(may, StringComparer.Ordinal);
                must = new(must, StringComparer.Ordinal);
                may.UnionWith(nodes[ordinal].GeneratedOwners);
                must.UnionWith(nodes[ordinal].GeneratedOwners);
                changed |= Replace(possibleOut, ordinal, may);
                changed |= Replace(definiteOut, ordinal, must);
            }
            if (!changed) { converged = true; break; }
        }
        if (!converged) return false;
        var edgeReleases = new Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>>();
        foreach (int ordinal in reachable)
            foreach (int target in nodes[ordinal].Successors)
            {
                var edge = new SemanticAsyncTaskOwnerEdge(ordinal, target);
                edgeReleases.Add(edge, possibleOut[ordinal]
                    .Intersect(releases.GetValueOrDefault(edge) ?? Array.Empty<string>(), StringComparer.Ordinal)
                    .OrderBy(id => id, StringComparer.Ordinal).ToArray());
            }
        result = new(Snapshot(possibleIn), Snapshot(possibleOut), Snapshot(definiteIn), Snapshot(definiteOut), edgeReleases);
        return true;
    }

    private static bool Replace(Dictionary<int, HashSet<string>> states, int ordinal, HashSet<string> value)
    {
        if (states[ordinal].SetEquals(value)) return false;
        states[ordinal] = value;
        return true;
    }

    private static IReadOnlyDictionary<int, IReadOnlyList<string>> Snapshot(Dictionary<int, HashSet<string>> states) =>
        states.ToDictionary(pair => pair.Key, pair => (IReadOnlyList<string>)pair.Value.OrderBy(id => id, StringComparer.Ordinal).ToArray());
}

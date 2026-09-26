using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Derived from the validated CFG, not another serialized authority. Possible
// owners drive cleanup; definite owners authorize source reads and aliases.
public sealed record SemanticAsyncTaskOwnerFlow(
    IReadOnlyDictionary<string, string> Producers,
    IReadOnlyDictionary<string, string> Aliases,
    IReadOnlyDictionary<int, IReadOnlyList<string>> PossibleAtEntry,
    IReadOnlyDictionary<int, IReadOnlyList<string>> PossibleAtExit,
    IReadOnlyDictionary<int, IReadOnlyList<string>> DefiniteAtEntry,
    IReadOnlyDictionary<int, IReadOnlyList<string>> DefiniteAtExit,
    bool RequiresGuards);

public static class SemanticAsyncTaskOwnership
{
    public static bool TryAnalyze(SemanticAsyncMethod method,
        IReadOnlyCollection<string> localIds, bool allowAliases,
        out SemanticAsyncTaskOwnerFlow? flow)
    {
        flow = null;
        if (method?.Segments is null || localIds is null || localIds.Count < 1
            || localIds.Count > SemanticAsyncInvocationValidator.MaximumTaskLocalsPerMethod) return false;
        HashSet<string> owned = new(localIds, StringComparer.Ordinal);
        if (owned.Count != localIds.Count || owned.Any(string.IsNullOrWhiteSpace)) return false;
        Dictionary<int, SemanticAsyncSegment> segments = new();
        Dictionary<int, int[]> edges = new();
        foreach (SemanticAsyncSegment segment in method.Segments)
        {
            if (segment?.Statements is null || !segments.TryAdd(segment.Ordinal, segment)
                || Successors(segment.Transfer) is not { } successors) return false;
            edges.Add(segment.Ordinal, successors);
        }
        if (!segments.ContainsKey(method.EntrySegmentOrdinal)
            || edges.Values.SelectMany(targets => targets).Any(target => !segments.ContainsKey(target))) return false;
        List<int> reachable = new();
        HashSet<int> visited = new();
        Queue<int> pending = new();
        pending.Enqueue(method.EntrySegmentOrdinal);
        while (pending.TryDequeue(out int ordinal))
        {
            if (!visited.Add(ordinal)) continue;
            reachable.Add(ordinal);
            foreach (int target in edges[ordinal]) pending.Enqueue(target);
        }
        Dictionary<string, SemanticAsyncStatement> declarations = new(StringComparer.Ordinal);
        Dictionary<int, HashSet<string>> generated = new();
        foreach (int ordinal in reachable)
        {
            HashSet<string> ids = new(StringComparer.Ordinal);
            foreach (SemanticAsyncStatement statement in segments[ordinal].Statements)
            {
                if (statement?.Operation is null) return false;
                if (statement.TargetSymbolId is { } id && owned.Contains(id))
                {
                    if (!declarations.TryAdd(id, statement)) return false;
                    ids.Add(id);
                }
            }
            generated.Add(ordinal, ids);
        }
        if (!owned.SetEquals(declarations.Keys)
            || method.TaskLocalSymbolIds is not null && !declarations
                .OrderBy(pair => pair.Value.Operation.Span.Start).Select(pair => pair.Key)
                .SequenceEqual(method.TaskLocalSymbolIds)) return false;
        Dictionary<string, string> roots = new(StringComparer.Ordinal);
        Dictionary<string, string> aliases = new(StringComparer.Ordinal);
        foreach (string id in owned)
        {
            HashSet<string> chain = new(StringComparer.Ordinal);
            string cursor = id;
            while (true)
            {
                if (!chain.Add(cursor) || !declarations.TryGetValue(cursor, out var declaration)) return false;
                if (declaration.Operation is { Kind: "invocation", SymbolId: { } callable })
                {
                    roots.Add(id, callable);
                    break;
                }
                if (!allowAliases || declaration.Operation is not
                    { Kind: "local_reference", SymbolId: { } source, Children.Count: 0 }) return false;
                if (cursor == id) aliases.Add(id, source);
                cursor = source;
            }
        }
        if (!SemanticAsyncTaskOwnerFlowSolver.TrySolve(method.EntrySegmentOrdinal, localIds,
                reachable.Select(ordinal => new SemanticAsyncTaskOwnerBlock(ordinal,
                    generated[ordinal].ToArray(), edges[ordinal])).ToArray(),
                new Dictionary<SemanticAsyncTaskOwnerEdge, IReadOnlyList<string>>(), out var states)) return false;
        var possibleIn = states!.PossibleAtEntry;
        var possibleOut = states.PossibleAtExit;
        var definiteIn = states.DefiniteAtEntry;
        var definiteOut = states.DefiniteAtExit;
        foreach (int ordinal in reachable)
        {
            HashSet<string> available = new(definiteIn[ordinal], StringComparer.Ordinal);
            foreach (SemanticAsyncStatement statement in segments[ordinal].Statements)
            {
                if (statement.TargetSymbolId is not { } id || !owned.Contains(id)) continue;
                // Re-entering a declaration would overwrite a still-owned
                // token. Loop-local retirement needs a separate scope plan.
                if (possibleIn[ordinal].Contains(id)
                    || aliases.TryGetValue(id, out string? source) && !available.Contains(source)) return false;
                available.Add(id);
            }
            if (segments[ordinal].AwaitSite?.TaskLocalSymbolId is { } awaited
                && !available.Contains(awaited)) return false;
        }
        flow = new(roots, aliases, possibleIn, possibleOut, definiteIn, definiteOut, reachable.Any(ordinal =>
                !possibleIn[ordinal].SequenceEqual(definiteIn[ordinal])
                    || !possibleOut[ordinal].SequenceEqual(definiteOut[ordinal])));
        return true;
    }

    private static int[]? Successors(SemanticAsyncControlTransfer? transfer) => transfer?.Kind switch
    {
        SemanticAsyncMethod.GotoTransferKind or SemanticAsyncMethod.EndCatchTransferKind
            or SemanticAsyncMethod.RethrowTransferKind or SemanticAsyncMethod.RaiseExceptionTransferKind
            when transfer.PrimaryTarget >= 0 => new[] { transfer.PrimaryTarget },
        SemanticAsyncMethod.AwaitTransferKind when transfer.PrimaryTarget >= 0 && transfer.SecondaryTarget >= -1
            && (transfer.CancellationTarget is null or >= 0) => new[] {
                transfer.PrimaryTarget, transfer.SecondaryTarget, transfer.CancellationTarget ?? -1 }
                .Where(target => target >= 0).Distinct().ToArray(),
        SemanticAsyncMethod.BranchTransferKind or SemanticAsyncMethod.CatchMatchTransferKind
            when transfer.PrimaryTarget >= 0 && transfer.SecondaryTarget >= 0 =>
            new[] { transfer.PrimaryTarget, transfer.SecondaryTarget }.Distinct().ToArray(),
        SemanticAsyncMethod.ReturnTransferKind or SemanticAsyncMethod.ThrowTransferKind
            or SemanticAsyncMethod.PropagateFaultTransferKind or SemanticAsyncMethod.PropagateCancellationTransferKind
            or SemanticAsyncMethod.PropagateExceptionTransferKind => Array.Empty<int>(),
        _ => null,
    };
}

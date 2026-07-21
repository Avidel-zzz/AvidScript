using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

internal static class SemanticReachabilityProjector
{
    public static SemanticReachability Project(
        IReadOnlyList<SemanticCallable> callables,
        IReadOnlyList<SemanticControlFlowGraph> controlFlowGraphs,
        IReadOnlyList<SemanticGameplayEventCallback> gameplayEventCallbacks)
    {
        Dictionary<string, SemanticCallable> callablesById = callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        string[] exportRootIds = callables
            .Where(callable => callable.Export is not null)
            .Select(callable => callable.MethodSymbolId)
            .ToArray();
        string[] rootIds = exportRootIds
            .Concat(gameplayEventCallbacks.Select(callback => callback.MethodSymbolId))
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToArray();
        if (rootIds.Length == 0)
        {
            string[] allCallableIds = callablesById.Keys.OrderBy(id => id, StringComparer.Ordinal).ToArray();
            return new SemanticReachability(
                "all_callables_compatibility",
                Array.Empty<string>(),
                allCallableIds,
                BuildReachableImports(allCallableIds, callablesById));
        }

        Dictionary<string, SemanticControlFlowGraph> graphsByMethodId = controlFlowGraphs.ToDictionary(
            graph => graph.MethodSymbolId,
            StringComparer.Ordinal);
        Dictionary<string, string[]> accessorsByAssociatedSymbolId = callables
            .Where(callable => callable.AssociatedSymbolId is not null)
            .GroupBy(callable => callable.AssociatedSymbolId!, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => group.Select(callable => callable.MethodSymbolId)
                    .OrderBy(id => id, StringComparer.Ordinal)
                    .ToArray(),
                StringComparer.Ordinal);
        SortedSet<string> pending = new(rootIds, StringComparer.Ordinal);
        HashSet<string> reachable = new(StringComparer.Ordinal);
        while (pending.Count > 0)
        {
            string current = pending.Min!;
            pending.Remove(current);
            if (!reachable.Add(current)
                || !graphsByMethodId.TryGetValue(current, out SemanticControlFlowGraph? graph))
            {
                continue;
            }

            foreach (SemanticOperation operation in graph.Blocks
                .Where(block => block.IsReachable)
                .SelectMany(EnumerateBlockOperations)
                .SelectMany(EnumerateOperations))
            {
                QueueTarget(operation.SymbolId, callablesById, accessorsByAssociatedSymbolId, reachable, pending);
                QueueTarget(operation.Conversion?.MethodSymbolId, callablesById, accessorsByAssociatedSymbolId, reachable, pending);
                QueueTarget(operation.InputConversion?.MethodSymbolId, callablesById, accessorsByAssociatedSymbolId, reachable, pending);
                QueueTarget(operation.OutputConversion?.MethodSymbolId, callablesById, accessorsByAssociatedSymbolId, reachable, pending);
            }
        }

        string[] reachableIds = reachable.OrderBy(id => id, StringComparer.Ordinal).ToArray();
        return new SemanticReachability(
            gameplayEventCallbacks.Count == 0 ? "export_roots" : "entrypoint_roots",
            rootIds,
            reachableIds,
            BuildReachableImports(reachableIds, callablesById));
    }

    private static IEnumerable<SemanticOperation> EnumerateBlockOperations(SemanticBasicBlock block)
    {
        foreach (SemanticOperation operation in block.Operations)
        {
            yield return operation;
        }

        if (block.BranchValue is not null)
        {
            yield return block.BranchValue;
        }
    }

    private static IEnumerable<SemanticOperation> EnumerateOperations(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in EnumerateOperations(child))
            {
                yield return descendant;
            }
        }
    }

    private static void QueueTarget(
        string? symbolId,
        IReadOnlyDictionary<string, SemanticCallable> callablesById,
        IReadOnlyDictionary<string, string[]> accessorsByAssociatedSymbolId,
        IReadOnlySet<string> reachable,
        ISet<string> pending)
    {
        if (symbolId is null)
        {
            return;
        }

        if (callablesById.ContainsKey(symbolId) && !reachable.Contains(symbolId))
        {
            pending.Add(symbolId);
        }

        if (accessorsByAssociatedSymbolId.TryGetValue(symbolId, out string[]? accessorIds))
        {
            foreach (string accessorId in accessorIds)
            {
                if (!reachable.Contains(accessorId))
                {
                    pending.Add(accessorId);
                }
            }
        }
    }

    private static SemanticReachableImport[] BuildReachableImports(
        IEnumerable<string> reachableIds,
        IReadOnlyDictionary<string, SemanticCallable> callablesById)
    {
        return reachableIds
            .Select(id => callablesById[id])
            .Where(callable => callable.Import is not null)
            .Select(callable => new SemanticReachableImport(
                callable.MethodSymbolId,
                callable.Import!.Module,
                callable.Import.Name))
            .OrderBy(import => import.Module, StringComparer.Ordinal)
            .ThenBy(import => import.Name, StringComparer.Ordinal)
            .ThenBy(import => import.MethodSymbolId, StringComparer.Ordinal)
            .ToArray();
    }
}

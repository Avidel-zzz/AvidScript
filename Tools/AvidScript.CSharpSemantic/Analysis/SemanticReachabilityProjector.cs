using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

internal static class SemanticReachabilityProjector
{
    public static SemanticReachability Project(
        IReadOnlyList<SemanticCallable> callables,
        IReadOnlyList<SemanticControlFlowGraph> controlFlowGraphs,
        IReadOnlyList<SemanticGameplayEventCallback> gameplayEventCallbacks,
        IReadOnlyList<SemanticDelegateEventCallback> delegateEventCallbacks,
        IReadOnlyList<SemanticContinuationCallback> continuationCallbacks,
        IReadOnlyList<SemanticAsyncMethod> asyncMethods)
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
            .Concat(delegateEventCallbacks.Select(callback => callback.MethodSymbolId))
            .Concat(continuationCallbacks.Select(callback => callback.MethodSymbolId))
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
        Dictionary<string, SemanticAsyncMethod> asyncMethodsById = asyncMethods.ToDictionary(
            method => method.MethodSymbolId,
            StringComparer.Ordinal);
        Dictionary<string, AssociatedAccessorTargets> accessorsByAssociatedSymbolId = callables
            .Where(callable => callable.AssociatedSymbolId is not null)
            .GroupBy(callable => callable.AssociatedSymbolId!, StringComparer.Ordinal)
            .ToDictionary(
                group => group.Key,
                group => new AssociatedAccessorTargets(
                    group.Select(callable => callable.MethodSymbolId)
                        .OrderBy(id => id, StringComparer.Ordinal)
                        .ToArray(),
                    group.Where(IsPropertyGetter)
                        .Select(callable => callable.MethodSymbolId)
                        .OrderBy(id => id, StringComparer.Ordinal)
                        .ToArray(),
                    group.Where(IsPropertySetter)
                        .Select(callable => callable.MethodSymbolId)
                        .OrderBy(id => id, StringComparer.Ordinal)
                        .ToArray()),
                StringComparer.Ordinal);
        SortedSet<string> pending = new(rootIds, StringComparer.Ordinal);
        HashSet<string> reachable = new(StringComparer.Ordinal);
        while (pending.Count > 0)
        {
            string current = pending.Min!;
            pending.Remove(current);
            if (!reachable.Add(current))
            {
                continue;
            }

            if (graphsByMethodId.TryGetValue(current, out SemanticControlFlowGraph? graph))
            {
                foreach (SemanticOperation operation in graph.Blocks
                    .Where(block => block.IsReachable)
                    .SelectMany(EnumerateBlockOperations))
                {
                    QueueOperationTargets(
                        operation,
                        PropertyAccess.Read,
                        callablesById,
                        accessorsByAssociatedSymbolId,
                        reachable,
                        pending);
                }
            }

            if (asyncMethodsById.TryGetValue(current, out SemanticAsyncMethod? asyncMethod))
            {
                foreach (SemanticOperation operation in EnumerateAsyncOperations(asyncMethod))
                {
                    QueueOperationTargets(
                        operation,
                        PropertyAccess.Read,
                        callablesById,
                        accessorsByAssociatedSymbolId,
                        reachable,
                        pending);
                }
            }
        }

        string[] reachableIds = reachable.OrderBy(id => id, StringComparer.Ordinal).ToArray();
        return new SemanticReachability(
            gameplayEventCallbacks.Count == 0
                && delegateEventCallbacks.Count == 0
                && continuationCallbacks.Count == 0
                ? "export_roots"
                : "entrypoint_roots",
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

    private static IEnumerable<SemanticOperation> EnumerateAsyncOperations(
        SemanticAsyncMethod method)
    {
        foreach (SemanticAsyncSegment segment in method.Segments)
        {
            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                yield return statement.Operation;
            }

            if (segment.Transfer?.Condition is { } condition)
            {
                yield return condition;
            }

            if (segment.AwaitSite is { } awaitSite)
            {
                foreach (SemanticOperation argument in awaitSite.Arguments)
                {
                    yield return argument;
                }
                if (awaitSite.CancellationToken is { } cancellationToken)
                {
                    yield return cancellationToken;
                }
            }
        }
    }

    private static void QueueOperationTargets(
        SemanticOperation operation,
        PropertyAccess propertyAccess,
        IReadOnlyDictionary<string, SemanticCallable> callablesById,
        IReadOnlyDictionary<string, AssociatedAccessorTargets> accessorsByAssociatedSymbolId,
        IReadOnlySet<string> reachable,
        ISet<string> pending)
    {
        QueueTarget(
            operation.SymbolId,
            callablesById,
            accessorsByAssociatedSymbolId,
            reachable,
            pending,
            includeAssociatedAccessors: operation.Kind != "property_reference");
        QueueTarget(
            operation.Conversion?.MethodSymbolId,
            callablesById,
            accessorsByAssociatedSymbolId,
            reachable,
            pending);
        QueueTarget(
            operation.InputConversion?.MethodSymbolId,
            callablesById,
            accessorsByAssociatedSymbolId,
            reachable,
            pending);
        QueueTarget(
            operation.OutputConversion?.MethodSymbolId,
            callablesById,
            accessorsByAssociatedSymbolId,
            reachable,
            pending);

        if (operation.Kind == "property_reference")
        {
            QueuePropertyAccessors(
                operation.SymbolId,
                propertyAccess,
                accessorsByAssociatedSymbolId,
                reachable,
                pending);
        }

        for (int index = 0; index < operation.Children.Count; ++index)
        {
            PropertyAccess childAccess = operation.Kind switch
            {
                "assignment" when index == 0 => PropertyAccess.Write,
                "compound_assignment" when index == 0 => PropertyAccess.ReadWrite,
                "increment_or_decrement" when index == 0 => PropertyAccess.ReadWrite,
                _ => PropertyAccess.Read,
            };
            QueueOperationTargets(
                operation.Children[index],
                childAccess,
                callablesById,
                accessorsByAssociatedSymbolId,
                reachable,
                pending);
        }
    }

    private static void QueueTarget(
        string? symbolId,
        IReadOnlyDictionary<string, SemanticCallable> callablesById,
        IReadOnlyDictionary<string, AssociatedAccessorTargets> accessorsByAssociatedSymbolId,
        IReadOnlySet<string> reachable,
        ISet<string> pending,
        bool includeAssociatedAccessors = true)
    {
        if (symbolId is null)
        {
            return;
        }

        if (callablesById.ContainsKey(symbolId) && !reachable.Contains(symbolId))
        {
            pending.Add(symbolId);
        }

        if (includeAssociatedAccessors
            && accessorsByAssociatedSymbolId.TryGetValue(symbolId, out AssociatedAccessorTargets? accessors))
        {
            QueueAccessorIds(accessors.All, reachable, pending);
        }
    }

    private static void QueuePropertyAccessors(
        string? propertySymbolId,
        PropertyAccess access,
        IReadOnlyDictionary<string, AssociatedAccessorTargets> accessorsByAssociatedSymbolId,
        IReadOnlySet<string> reachable,
        ISet<string> pending)
    {
        if (propertySymbolId is null
            || !accessorsByAssociatedSymbolId.TryGetValue(propertySymbolId, out AssociatedAccessorTargets? accessors))
        {
            return;
        }

        if (access is PropertyAccess.Read or PropertyAccess.ReadWrite)
        {
            QueueAccessorIds(accessors.Getters, reachable, pending);
        }

        if (access is PropertyAccess.Write or PropertyAccess.ReadWrite)
        {
            QueueAccessorIds(accessors.Setters, reachable, pending);
        }
    }

    private static void QueueAccessorIds(
        IEnumerable<string> accessorIds,
        IReadOnlySet<string> reachable,
        ISet<string> pending)
    {
        foreach (string accessorId in accessorIds)
        {
            if (!reachable.Contains(accessorId))
            {
                pending.Add(accessorId);
            }
        }
    }

    private static bool IsPropertyGetter(SemanticCallable callable)
    {
        return !string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal);
    }

    private static bool IsPropertySetter(SemanticCallable callable)
    {
        return string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
            && callable.Parameters.Count > 0;
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

    private enum PropertyAccess
    {
        Read,
        Write,
        ReadWrite,
    }

    private sealed record AssociatedAccessorTargets(
        IReadOnlyList<string> All,
        IReadOnlyList<string> Getters,
        IReadOnlyList<string> Setters);
}

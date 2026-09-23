using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Derived planning data only. The Guest cannot execute exception flows yet.
public sealed record SemanticLanguageErrorEffectPlan(
    IReadOnlyList<string> OutcomeMethodIds);

public static class SemanticLanguageErrorEffectPlanner
{
    public static bool TryBuild(SemanticDocument document, out SemanticLanguageErrorEffectPlan? plan)
    {
        plan = null;
        if (document is null || document.ExceptionFlows is not { Count: > 0 }
            || document.Callables is null || document.Methods is null
            || document.Callables.Count > 4096 || document.Methods.Count > 4096
            || document.Callables.Any(callable => callable is null
                || string.IsNullOrWhiteSpace(callable.MethodSymbolId))
            || !SemanticExceptionFlowContractValidator.IsValid(document))
            return false;

        HashSet<string> callableIds = document.Callables
            .Select(callable => callable.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        if (callableIds.Count != document.Callables.Count) return false;
        Dictionary<string, string[]> accessors = document.Callables
            .Where(callable => callable.AssociatedSymbolId is not null)
            .GroupBy(callable => callable.AssociatedSymbolId!, StringComparer.Ordinal)
            .ToDictionary(group => group.Key,
                group => group.Select(callable => callable.MethodSymbolId)
                    .OrderBy(id => id, StringComparer.Ordinal).ToArray(),
                StringComparer.Ordinal);
        Dictionary<string, HashSet<string>> callees = new(StringComparer.Ordinal);
        int operationCount = 0;
        foreach (SemanticMethodBody body in document.Methods)
        {
            if (body is null || body.Root is null
                || !callableIds.Contains(body.MethodSymbolId)
                || callees.ContainsKey(body.MethodSymbolId)) return false;
            HashSet<string> targets = new(StringComparer.Ordinal);
            Stack<SemanticOperation> pending = new();
            pending.Push(body.Root);
            while (pending.TryPop(out SemanticOperation? operation))
            {
                if (operation is null || operation.Children is null
                    || ++operationCount > 100_000) return false;
                // A non-direct call can reach an override that this artifact has not
                // enumerated. Do not publish a partial effect closure for it.
                if (operation.Kind == "invocation"
                    && operation.Dispatch?.Kind is not ("static" or "direct"))
                    return false;
                AddTarget(operation.SymbolId, targets, callableIds);
                AddTarget(operation.Conversion?.MethodSymbolId, targets, callableIds);
                AddTarget(operation.InputConversion?.MethodSymbolId, targets, callableIds);
                AddTarget(operation.OutputConversion?.MethodSymbolId, targets, callableIds);
                if (operation.Kind == "property_reference" && operation.SymbolId is { } propertyId
                    && accessors.TryGetValue(propertyId, out string[]? propertyAccessors))
                    targets.UnionWith(propertyAccessors);
                foreach (SemanticOperation child in operation.Children) pending.Push(child);
            }
            callees.Add(body.MethodSymbolId, targets);
        }

        Dictionary<string, List<string>> callersByTarget = new(StringComparer.Ordinal);
        foreach ((string caller, HashSet<string> targets) in callees)
        {
            foreach (string target in targets)
            {
                if (!callersByTarget.TryGetValue(target, out List<string>? callers))
                    callersByTarget.Add(target, callers = new());
                callers.Add(caller);
            }
        }
        HashSet<string> affected = document.ExceptionFlows
            .Select(flow => flow.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        Queue<string> pendingCallers = new(affected.OrderBy(id => id, StringComparer.Ordinal));
        while (pendingCallers.TryDequeue(out string? target))
            if (callersByTarget.TryGetValue(target, out List<string>? callers))
                foreach (string caller in callers)
                    if (affected.Add(caller)) pendingCallers.Enqueue(caller);
        plan = new(affected.OrderBy(id => id, StringComparer.Ordinal).ToArray());
        return true;
    }

    private static void AddTarget(string? symbolId, ISet<string> targets, ISet<string> callableIds)
    {
        if (symbolId is not null && callableIds.Contains(symbolId)) targets.Add(symbolId);
    }
}

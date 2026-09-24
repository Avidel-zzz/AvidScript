using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// A source-level dispatch decision. Guest lowering must still execute the
// selected cleanup blocks and transfer ownership of the live error root.
public sealed record SemanticExceptionDispatchResolution(
    IReadOnlyList<int> FinallyRegionOrdinals,
    int? HandlerOrdinal);

public static class SemanticExceptionDispatchResolver
{
    public static bool TryResolve(
        SemanticDocument document,
        string methodSymbolId,
        int sourceBlockOrdinal,
        string errorTypeId,
        out SemanticExceptionDispatchResolution? resolution)
    {
        resolution = null;
        if (document is null || string.IsNullOrWhiteSpace(methodSymbolId)
            || string.IsNullOrWhiteSpace(errorTypeId)
            || !SemanticExceptionFlowContractValidator.IsValid(document)
            || !SemanticClassContractValidator.IsValid(document)
            || document.ExceptionFlows!.SingleOrDefault(flow =>
                flow.MethodSymbolId == methodSymbolId) is not { } flow
            || sourceBlockOrdinal < 0 || sourceBlockOrdinal >= flow.Blocks!.Count
            || !SemanticExceptionDispatchPlanner.TryBuild(flow, out var plan)
            || plan is null)
            return false;

        Dictionary<string, SemanticClassType> classes = document.ClassTypes
            .ToDictionary(type => type.TypeId, StringComparer.Ordinal);
        if (!IsAssignable(errorTypeId, "type:global::System.Exception", classes))
            return false;

        List<int> cleanup = new();
        foreach (SemanticExceptionDispatchStep step in plan.Routes[sourceBlockOrdinal].Steps)
        {
            if (step.Kind == "finally")
            {
                cleanup.Add(step.RegionOrdinal);
                continue;
            }
            if (step.Kind != "catch") return false;
            foreach (int handlerOrdinal in step.HandlerOrdinals)
            {
                SemanticCatchHandler handler = flow.Catches[handlerOrdinal];
                if (handler.ExceptionTypeId is null
                    || IsAssignable(errorTypeId, handler.ExceptionTypeId, classes))
                {
                    resolution = new(cleanup, handlerOrdinal);
                    return true;
                }
            }
        }
        resolution = new(cleanup, null);
        return true;
    }

    private static bool IsAssignable(
        string actualTypeId,
        string expectedTypeId,
        IReadOnlyDictionary<string, SemanticClassType> classes)
    {
        if (!classes.TryGetValue(actualTypeId, out SemanticClassType? current)
            || !classes.ContainsKey(expectedTypeId))
            return false;
        // The class contract has already rejected cycles and dangling bases.
        while (current is not null)
        {
            if (current.TypeId == expectedTypeId) return true;
            current = current.BaseTypeId is { } parent ? classes[parent] : null;
        }
        return false;
    }
}

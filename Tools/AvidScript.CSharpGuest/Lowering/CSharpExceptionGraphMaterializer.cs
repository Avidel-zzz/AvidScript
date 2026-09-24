using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLocalThrowSite(int BlockOrdinal, SemanticThrowSite Site);

// Internal bridge for exception methods whose ordinary block operations can be
// lowered unchanged. The exceptional call edges are added by the outcome pass;
// local throw return placeholders are replaced before publishing the module.
internal static class CSharpExceptionGraphMaterializer
{
    public static bool TryBuild(SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        error = null;
        if (flow.Catches.Count == 0
            || flow.Catches.Any(handler => handler.HasFilter
                || handler.ExceptionVariableSymbolId is not null)
            || flow.Regions.Any(region => region.Kind is not
                ("root" or "local_lifetime" or "try" or "try_and_catch" or "catch"))
            || flow.Blocks is not { Count: > 0 } blocks
            || flow.Branches.Any(branch => branch.Semantics is not
                ("regular" or "return" or "throw")
                || branch.Semantics != "throw" && branch.DestinationBlockOrdinal < 0)
            || blocks.Any(block => block.Operations.Any(operation => !Supported(operation))
                || block.BranchValue is { } value && !Supported(value)))
            return Fail("The catch method needs unsupported throw, cleanup, filter, variable, or block operations.", out error);

        List<CSharpLocalThrowSite> projectedThrows = new();
        foreach (SemanticExceptionBranch branch in flow.Branches.Where(item => item.Semantics == "throw"))
        {
            SemanticExceptionBlock block = blocks[branch.SourceBlockOrdinal];
            SemanticOperation? expression = block.BranchValue;
            SemanticThrowSite[] matches = flow.Throws.Where(site => site.Kind == "throw"
                && site.ExceptionTypeId == CSharpThrowProducerLowerer.ExceptionTypeId
                && expression is not null && site.Span.Start <= expression.Span.Start
                && expression.Span.End <= site.Span.End).ToArray();
            if (branch.DestinationBlockOrdinal != -1
                || flow.Branches.Count(item => item.SourceBlockOrdinal == block.Ordinal) != 1
                || block.Operations.Count != 0
                || expression is not { Kind: "object_creation", IsSupported: true }
                || expression.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                || expression.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
                || expression.Children.Count != 0 || matches.Length != 1
                || branch.FinallyRegionOrdinals.Count != 0)
                return Fail("Only a zero-argument System.Exception local throw without cleanup is executable.", out error);
            projectedThrows.Add(new(block.Ordinal, matches[0]));
        }
        if (projectedThrows.Count != flow.Throws.Count
            || projectedThrows.Select(item => item.Site.Span.Start).Distinct().Count()
                != projectedThrows.Count)
            return Fail("Every local throw needs one distinct executable source block.", out error);

        SemanticControlFlowEdge[] edges = flow.Branches.Select(branch =>
            new SemanticControlFlowEdge(branch.SourceBlockOrdinal,
                branch.Semantics == "throw" ? blocks.Count - 1 : branch.DestinationBlockOrdinal,
                branch.Kind, branch.Semantics == "throw" ? "return" : branch.Semantics)).ToArray();
        IReadOnlySet<int> throwBlocks = projectedThrows.Select(item => item.BlockOrdinal).ToHashSet();
        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.IsReachable, block.ConditionKind, block.Operations,
                throwBlocks.Contains(block.Ordinal) ? ZeroPlaceholder(block.BranchValue!.Span)
                    : block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        localThrows = projectedThrows;
        return true;
    }

    private static SemanticOperation ZeroPlaceholder(SemanticSpan span) => new(
        "literal", true, null, false, false, false, false,
        "type:int32", null, Array.Empty<string>(), new SemanticConstant("int32", "0"),
        null, null, null, null, span, Array.Empty<SemanticOperation>());

    private static bool Supported(SemanticOperation operation) =>
        operation.IsSupported && operation.Children.All(Supported);

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}

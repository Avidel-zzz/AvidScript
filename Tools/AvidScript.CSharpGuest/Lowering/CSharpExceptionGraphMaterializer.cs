using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLocalThrowSite(
    int BlockOrdinal, SemanticThrowSite Site, int? CleanupBlockOrdinal = null);

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
        if (flow.Catches.Count == 0 && flow.Regions.Any(region => region.Kind == "finally"))
            return TryBuildDirectThrowFinally(flow, out graph, out localThrows, out error);
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

    private static bool TryBuildDirectThrowFinally(
        SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        error = null;
        if (flow.Catches.Count != 0 || flow.Throws.Count != 1
            || flow.Blocks is not { Count: 4 } blocks
            || flow.Regions.Count != 4 || flow.Branches.Count != 3
            || !SemanticExceptionDispatchPlanner.TryBuild(flow, out var dispatch)
            || dispatch is null)
            return Fail("Direct throw cleanup needs one validated linear finally.", out error);
        SemanticExceptionRegion[] finallyRegions = flow.Regions
            .Where(region => region.Kind == "finally").ToArray();
        SemanticExceptionBranch[] throws = flow.Branches
            .Where(branch => branch.Semantics == "throw").ToArray();
        if (finallyRegions.Length != 1 || throws.Length != 1
            || flow.Regions[0].Kind != "root"
            || flow.Regions[1].Kind != "try_and_finally"
            || flow.Regions[1].ParentOrdinal != 0
            || flow.Regions[2].Kind != "try" || flow.Regions[2].ParentOrdinal != 1
            || flow.Regions[3].Kind != "finally" || flow.Regions[3].ParentOrdinal != 1)
            return Fail("Direct throw cleanup has an unsupported region layout.", out error);
        int throwOrdinal = throws[0].SourceBlockOrdinal;
        int cleanupOrdinal = finallyRegions[0].FirstBlockOrdinal;
        if (throwOrdinal != 1 || cleanupOrdinal != 2
            || finallyRegions[0].LastBlockOrdinal != cleanupOrdinal
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == throwOrdinal) != 1
            || throws[0].DestinationBlockOrdinal != -1
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 0
                && branch.DestinationBlockOrdinal == throwOrdinal
                && branch.Semantics == "regular") != 1
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == cleanupOrdinal
                && branch.Semantics == "structured_exception_handling"
                && branch.DestinationBlockOrdinal == -1) != 1
            || dispatch.Routes[throwOrdinal].Steps.Count != 1
            || dispatch.Routes[throwOrdinal].Steps[0].Kind != "finally"
            || dispatch.Routes[throwOrdinal].Steps[0].RegionOrdinal
                != finallyRegions[0].Ordinal)
            return Fail("Direct throw cleanup has an unsupported control-flow route.", out error);
        SemanticExceptionBlock throwBlock = blocks[throwOrdinal];
        SemanticExceptionBlock cleanupBlock = blocks[cleanupOrdinal];
        SemanticOperation? expression = throwBlock.BranchValue;
        SemanticThrowSite site = flow.Throws[0];
        if (site.Kind != "throw" || site.ExceptionTypeId != CSharpThrowProducerLowerer.ExceptionTypeId
            || blocks[0].Operations.Count != 0 || blocks[0].BranchValue is not null
            || blocks[3].Operations.Count != 0 || blocks[3].BranchValue is not null
            || throwBlock.EnclosingRegionOrdinal != 2
            || cleanupBlock.EnclosingRegionOrdinal != 3
            || throwBlock.Operations.Count != 0
            || expression is not { Kind: "object_creation", IsSupported: true }
            || expression.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
            || expression.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
            || expression.Children.Count != 0
            || site.Span.Start > expression.Span.Start || expression.Span.End > site.Span.End
            || cleanupBlock.BranchValue is not null || cleanupBlock.Operations.Count == 0
            || cleanupBlock.Operations.Any(operation => !Supported(operation)
                || Descendants(operation).Any(item => item.Kind is
                    "conditional" or "switch" or "branch" or "loop" or "await" or "try"
                    or "throw" or "invocation")))
            return Fail("Direct throw cleanup needs a zero-argument exception and linear synchronous finally.",
                out error);

        SemanticControlFlowEdge[] edges =
        {
            new(0, throwOrdinal, "fallthrough", "regular"),
            new(throwOrdinal, cleanupOrdinal, "fallthrough", "regular"),
            new(cleanupOrdinal, blocks.Count - 1, "fallthrough", "return"),
        };
        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.Ordinal == blocks.Count - 1 || block.IsReachable,
                block.ConditionKind, block.Operations,
                block.Ordinal == throwOrdinal ? null
                    : block.Ordinal == cleanupOrdinal ? ZeroPlaceholder(site.Span)
                        : block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        localThrows = new[] { new CSharpLocalThrowSite(throwOrdinal, site, cleanupOrdinal) };
        return true;
    }

    private static IEnumerable<SemanticOperation> Descendants(SemanticOperation root)
    {
        Stack<SemanticOperation> pending = new();
        pending.Push(root);
        while (pending.TryPop(out SemanticOperation? operation))
        {
            yield return operation;
            foreach (SemanticOperation child in operation.Children) pending.Push(child);
        }
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

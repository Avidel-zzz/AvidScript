using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLocalThrowSite(
    int BlockOrdinal, SemanticThrowSite Site, int? CleanupBlockOrdinal = null,
    int? ReplacesThrowBlockOrdinal = null);
internal sealed record CSharpRethrowSite(
    int BlockOrdinal, int HandlerOrdinal, SemanticThrowSite Site);

// Internal bridge for exception methods whose ordinary block operations can be
// lowered unchanged. The exceptional call edges are added by the outcome pass;
// local throw return placeholders are replaced before publishing the module.
internal static class CSharpExceptionGraphMaterializer
{
    public static bool TryBuild(SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out IReadOnlyList<CSharpRethrowSite> rethrows,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        rethrows = Array.Empty<CSharpRethrowSite>();
        error = null;
        if (flow.Catches.Count == 0 && flow.Regions.Any(region => region.Kind == "finally"))
            return TryBuildDirectThrowFinally(flow, out graph, out localThrows, out error);
        if (flow.Catches.Count == 0
            || flow.Catches.Any(handler => handler.HasFilter
                || handler.ExceptionVariableSymbolId is not null
                    && handler.ExceptionTypeId != CSharpThrowProducerLowerer.ExceptionTypeId)
            || flow.Regions.Any(region => region.Kind is not
                ("root" or "local_lifetime" or "try" or "try_and_catch" or "catch"))
            || flow.Blocks is not { Count: > 0 } blocks
            || flow.Branches.Any(branch => branch.Semantics is not
                ("regular" or "return" or "throw" or "rethrow")
                || branch.Semantics is not ("throw" or "rethrow")
                    && branch.DestinationBlockOrdinal < 0))
            return Fail("The catch method needs unsupported throw, cleanup, filter, variable, or block operations.", out error);

        Dictionary<int, SemanticCatchHandler> boundHandlers = new();
        foreach (SemanticCatchHandler handler in flow.Catches.Where(item =>
            item.ExceptionVariableSymbolId is not null))
        {
            int first = flow.Regions[handler.RegionOrdinal].FirstBlockOrdinal;
            if (first < 0 || first >= blocks.Count
                || !boundHandlers.TryAdd(first, handler)
                || !IsCatchBindingBlock(blocks[first], handler))
                return Fail($"Catch handler {handler.Ordinal} in '{flow.MethodSymbolId}' needs one source-derived exception assignment.", out error);
        }
        if (blocks.Any(block => !boundHandlers.ContainsKey(block.Ordinal)
                && block.Operations.Any(operation => !Supported(operation))
                || block.BranchValue is { } value && !Supported(value)))
            return Fail("The catch method has unsupported block operations.", out error);

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
        List<CSharpRethrowSite> projectedRethrows = new();
        if (!SemanticExceptionDispatchPlanner.TryBuild(flow, out var dispatch)
            || dispatch is null)
            return Fail("The catch method has an invalid exception dispatch route.", out error);
        foreach (SemanticExceptionBranch branch in flow.Branches.Where(item => item.Semantics == "rethrow"))
        {
            SemanticExceptionBlock block = blocks[branch.SourceBlockOrdinal];
            SemanticCatchHandler[] handlers = flow.Catches.Where(handler =>
                handler.RegionOrdinal == block.EnclosingRegionOrdinal
                && flow.Regions[handler.RegionOrdinal].FirstBlockOrdinal == block.Ordinal
                && flow.Regions[handler.RegionOrdinal].LastBlockOrdinal == block.Ordinal).ToArray();
            SemanticThrowSite[] sites = flow.Throws.Where(site => site.Kind == "rethrow"
                && handlers.Length == 1 && handlers[0].Span.Start <= site.Span.Start
                && site.Span.End <= handlers[0].Span.End).ToArray();
            if (branch.DestinationBlockOrdinal != -1
                || flow.Branches.Count(item => item.SourceBlockOrdinal == block.Ordinal) != 1
                || block.Operations.Count != 0 && !boundHandlers.ContainsKey(block.Ordinal)
                || block.BranchValue is not null
                || handlers.Length != 1 || sites.Length != 1
                || dispatch.Routes[block.Ordinal].Steps.Any(step => step.Kind != "catch"))
                return Fail("Only a direct catch rethrow without cleanup is executable.", out error);
            projectedRethrows.Add(new(block.Ordinal, handlers[0].Ordinal, sites[0]));
        }
        if (projectedThrows.Count + projectedRethrows.Count != flow.Throws.Count
            || projectedThrows.Select(item => item.Site.Span.Start).Distinct().Count()
                != projectedThrows.Count
            || projectedRethrows.Select(item => item.Site.Span.Start).Distinct().Count()
                != projectedRethrows.Count)
            return Fail("Every local throw needs one distinct executable source block.", out error);

        SemanticControlFlowEdge[] edges = flow.Branches.Select(branch =>
            new SemanticControlFlowEdge(branch.SourceBlockOrdinal,
                branch.Semantics is "throw" or "rethrow"
                    ? blocks.Count - 1 : branch.DestinationBlockOrdinal,
                branch.Kind, branch.Semantics is "throw" or "rethrow"
                    ? "return" : branch.Semantics)).ToArray();
        IReadOnlySet<int> throwBlocks = projectedThrows.Select(item => item.BlockOrdinal).ToHashSet();
        IReadOnlyDictionary<int, SemanticSpan> rethrowSpans = projectedRethrows
            .ToDictionary(item => item.BlockOrdinal, item => item.Site.Span);
        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.IsReachable, block.ConditionKind,
                boundHandlers.ContainsKey(block.Ordinal)
                    ? Array.Empty<SemanticOperation>() : block.Operations,
                throwBlocks.Contains(block.Ordinal) ? ZeroPlaceholder(block.BranchValue!.Span)
                    : rethrowSpans.TryGetValue(block.Ordinal, out SemanticSpan? span)
                        ? ZeroPlaceholder(span)
                    : block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        localThrows = projectedThrows;
        rethrows = projectedRethrows;
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
        if (flow.Catches.Count != 0 || flow.Throws.Count is not (1 or 2)
            || flow.Blocks is not { Count: 4 } blocks
            || flow.Regions.Count != 4 || flow.Branches.Count != 3
            || !SemanticExceptionDispatchPlanner.TryBuild(flow, out var dispatch)
            || dispatch is null)
            return Fail("Direct throw cleanup needs one validated linear finally.", out error);
        SemanticExceptionRegion[] finallyRegions = flow.Regions
            .Where(region => region.Kind == "finally").ToArray();
        SemanticExceptionBranch[] throws = flow.Branches
            .Where(branch => branch.Semantics == "throw").ToArray();
        bool replacesError = flow.Throws.Count == 2;
        if (finallyRegions.Length != 1 || throws.Length != (replacesError ? 2 : 1)
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
                && branch.Semantics == (replacesError ? "throw" : "structured_exception_handling")
                && branch.DestinationBlockOrdinal == -1) != 1
            || dispatch.Routes[throwOrdinal].Steps.Count != 1
            || dispatch.Routes[throwOrdinal].Steps[0].Kind != "finally"
            || dispatch.Routes[throwOrdinal].Steps[0].RegionOrdinal
                != finallyRegions[0].Ordinal
            || replacesError && (throws[1].SourceBlockOrdinal != cleanupOrdinal
                || dispatch.Routes[cleanupOrdinal].Steps.Count != 0))
            return Fail("Direct throw cleanup has an unsupported control-flow route.", out error);
        SemanticExceptionBlock throwBlock = blocks[throwOrdinal];
        SemanticExceptionBlock cleanupBlock = blocks[cleanupOrdinal];
        SemanticOperation? expression = throwBlock.BranchValue;
        SemanticOperation? replacementExpression = cleanupBlock.BranchValue;
        SemanticThrowSite[] originalSites = flow.Throws.Where(item => expression is not null
            && item.Span.Start <= expression.Span.Start
            && expression.Span.End <= item.Span.End).ToArray();
        SemanticThrowSite[] replacementSites = flow.Throws.Where(item =>
            replacementExpression is not null
            && item.Span.Start <= replacementExpression.Span.Start
            && replacementExpression.Span.End <= item.Span.End).ToArray();
        SemanticThrowSite? site = originalSites.Length == 1 ? originalSites[0] : null;
        SemanticThrowSite? replacement = replacementSites.Length == 1
            ? replacementSites[0] : null;
        if (site is null || site.Kind != "throw"
            || site.ExceptionTypeId != CSharpThrowProducerLowerer.ExceptionTypeId
            || originalSites.Length != 1
            || replacementSites.Length != (replacesError ? 1 : 0)
            || replacesError && replacement!.Span == site.Span
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
            || !replacesError && cleanupBlock.Operations.Count == 0
            || !replacesError && replacementExpression is not null
            || replacesError && (replacement is null || replacement.Kind != "throw"
                || replacement.ExceptionTypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                || replacementExpression is not { Kind: "object_creation", IsSupported: true }
                || replacementExpression.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                || replacementExpression.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
                || replacementExpression.Children.Count != 0
                || replacement.Span.Start > replacementExpression.Span.Start
                || replacementExpression.Span.End > replacement.Span.End)
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
        localThrows = replacesError
            ? new[]
            {
                new CSharpLocalThrowSite(throwOrdinal, site, cleanupOrdinal),
                new CSharpLocalThrowSite(cleanupOrdinal, replacement!,
                    ReplacesThrowBlockOrdinal: throwOrdinal),
            }
            : new[] { new CSharpLocalThrowSite(throwOrdinal, site, cleanupOrdinal) };
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

    private static bool IsCatchBindingBlock(
        SemanticExceptionBlock block, SemanticCatchHandler handler)
    {
        if (block.Operations.Count == 0) return true;
        if (block.Operations.Count != 1
            || block.Operations[0] is not { Kind: "assignment", IsSupported: true } assignment
            || assignment.Children.Count != 2)
            return false;
        SemanticOperation target = assignment.Children[0];
        SemanticOperation value = assignment.Children[1];
        return target is { Kind: "local_reference", IsSupported: true }
            && target.SymbolId == handler.ExceptionVariableSymbolId
            && target.TypeId == handler.ExceptionTypeId
            && target.Children.Count == 0
            && value is { Kind: "roslyn:CaughtException", IsSupported: false }
            && value.TypeId == handler.ExceptionTypeId
            && value.Children.Count == 0
            && handler.Span.Start <= assignment.Span.Start
            && assignment.Span.End <= handler.Span.End;
    }

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}

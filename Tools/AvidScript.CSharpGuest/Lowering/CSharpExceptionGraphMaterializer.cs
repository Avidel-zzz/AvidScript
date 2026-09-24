using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLocalThrowSite(
    int BlockOrdinal, SemanticThrowSite Site,
    IReadOnlyList<int>? CleanupBlockOrdinals = null,
    int? ReplacesThrowBlockOrdinal = null,
    CSharpBranchingCleanup? BranchingCleanup = null);
internal sealed record CSharpBranchingCleanup(
    int EntryBlockOrdinal, int ExitBlockOrdinal,
    IReadOnlyList<int> BlockOrdinals);
internal sealed record CSharpNormalReturnCleanupSite(
    int BlockOrdinal, int CleanupBlockOrdinal,
    CSharpBranchingCleanup? BranchingCleanup = null);
internal sealed record CSharpRethrowSite(
    int BlockOrdinal, int HandlerOrdinal, SemanticThrowSite Site,
    int? CleanupBlockOrdinal = null,
    CSharpBranchingCleanup? BranchingCleanup = null);

// Internal bridge for exception methods whose ordinary block operations can be
// lowered unchanged. The exceptional call edges are added by the outcome pass;
// local throw return placeholders are replaced before publishing the module.
internal static class CSharpExceptionGraphMaterializer
{
    public static bool TryBuild(SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out IReadOnlyList<CSharpNormalReturnCleanupSite> normalReturns,
        out IReadOnlyList<CSharpRethrowSite> rethrows,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        normalReturns = Array.Empty<CSharpNormalReturnCleanupSite>();
        rethrows = Array.Empty<CSharpRethrowSite>();
        error = null;
        if (flow.Catches.Count != 0 && flow.Regions.Any(region => region.Kind == "finally"))
            return TryBuildCatchFinally(flow, out graph, out localThrows,
                out normalReturns, out rethrows, out error);
        if (flow.Catches.Count == 0 && flow.Regions.Any(region => region.Kind == "finally"))
        {
            if (flow.Regions.Count(region => region.Kind == "finally") != 1)
                return TryBuildNestedDirectThrowFinally(flow, out graph, out localThrows, out error);
            if (flow.Regions.First(region => region.Kind == "finally") is { } cleanup
                && cleanup.LastBlockOrdinal > cleanup.FirstBlockOrdinal
                && flow.Regions.Count == 4 && flow.Regions[2].Kind == "try"
                && flow.Regions[2].FirstBlockOrdinal == flow.Regions[2].LastBlockOrdinal)
                return TryBuildBranchingCleanup(flow, out graph, out localThrows, out error);
            return flow.Blocks is { Count: > 4 } && flow.Throws.Count > 0
                ? TryBuildBranchingThrowFinally(flow, out graph, out localThrows,
                    out normalReturns, out error)
                : TryBuildDirectThrowFinally(flow, out graph, out localThrows, out error);
        }
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

    private static bool TryBuildBranchingCleanup(
        SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        error = null;
        if (flow.Catches.Count != 0 || flow.Throws.Count != 1
            || flow.Regions.Count != 4
            || flow.Regions[0].Kind != "root"
            || flow.Regions[1].Kind != "try_and_finally"
            || flow.Regions[1].ParentOrdinal != 0
            || flow.Regions[2].Kind != "try"
            || flow.Regions[2].ParentOrdinal != 1
            || flow.Regions[3].Kind != "finally"
            || flow.Regions[3].ParentOrdinal != 1
            || flow.Blocks is not { } blocks
            || !SemanticExceptionDispatchPlanner.TryBuild(flow, out var dispatch)
            || dispatch is null)
            return Fail("Branching cleanup needs one direct throw and one finally region.",
                out error);

        SemanticExceptionRegion cleanup = flow.Regions[3];
        int entry = cleanup.FirstBlockOrdinal, last = cleanup.LastBlockOrdinal;
        if (entry != 2 || last is < 4 or > 17 || blocks.Count != last + 2
            || flow.Regions[2].FirstBlockOrdinal != 1
            || flow.Regions[2].LastBlockOrdinal != 1
            || blocks[0].Operations.Count != 0 || blocks[0].BranchValue is not null
            || blocks[^1].Operations.Count != 0 || blocks[^1].BranchValue is not null
            || blocks[1].EnclosingRegionOrdinal != flow.Regions[2].Ordinal
            || blocks[1].Operations.Count != 0
            || blocks[1].BranchValue is not { Kind: "object_creation", IsSupported: true } expression
            || expression.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
            || expression.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
            || expression.Children.Count != 0
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 0
                && branch.DestinationBlockOrdinal == 1
                && branch.Semantics == "regular") != 1
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 1
                && branch.DestinationBlockOrdinal == -1
                && branch.Semantics == "throw") != 1
            || dispatch.Routes[1].Steps.Count != 1
            || dispatch.Routes[1].Steps[0].Kind != "finally"
            || dispatch.Routes[1].Steps[0].RegionOrdinal != cleanup.Ordinal)
            return Fail("Branching cleanup has an unsupported source route.", out error);
        SemanticThrowSite[] sites = flow.Throws.Where(site => site.Kind == "throw"
            && site.ExceptionTypeId == CSharpThrowProducerLowerer.ExceptionTypeId
            && site.Span.Start <= expression.Span.Start
            && expression.Span.End <= site.Span.End).ToArray();
        if (sites.Length != 1)
            return Fail("Branching cleanup needs one source-backed exception.", out error);

        List<SemanticControlFlowEdge> edges = new()
        {
            new(0, 1, "fallthrough", "regular"),
            new(1, entry, "fallthrough", "regular"),
        };
        for (int ordinal = entry; ordinal <= last; ++ordinal)
        {
            SemanticExceptionBlock block = blocks[ordinal];
            SemanticExceptionBranch[] outgoing = flow.Branches.Where(branch =>
                branch.SourceBlockOrdinal == ordinal).ToArray();
            if (block.EnclosingRegionOrdinal != cleanup.Ordinal
                || block.Operations.Any(operation => !Supported(operation)
                    || Descendants(operation).Any(item => item.Kind is
                        "conditional" or "switch" or "branch" or "loop" or "await"
                        or "try" or "throw" or "invocation")))
                return Fail("Branching cleanup has an unsupported operation.", out error);
            if (ordinal == last)
            {
                if (block.ConditionKind != "none" || block.BranchValue is not null
                    || outgoing.Length != 1
                    || outgoing[0].DestinationBlockOrdinal != -1
                    || outgoing[0].Semantics != "structured_exception_handling")
                    return Fail("Branching cleanup needs one final exit.", out error);
                edges.Add(new(ordinal, blocks.Count - 1, "fallthrough", "return"));
                continue;
            }
            if (outgoing.Length == 2)
            {
                if (block.Operations.Count != 0 || block.ConditionKind == "none"
                    || block.BranchValue is not { } condition
                    || !PureThrowDecision(condition)
                    || outgoing.Count(branch => branch.Kind == "fallthrough") != 1
                    || outgoing.Count(branch => branch.Kind == "conditional") != 1
                    || outgoing.Select(branch => branch.DestinationBlockOrdinal)
                        .Distinct().Count() != 2)
                    return Fail("Branching cleanup needs a pure conditional.", out error);
            }
            else if (outgoing.Length != 1 || block.ConditionKind != "none"
                || block.BranchValue is not null || outgoing[0].Kind != "fallthrough")
                return Fail("Branching cleanup needs bounded forward blocks.", out error);
            if (outgoing.Any(branch => branch.Semantics != "regular"
                || branch.DestinationBlockOrdinal <= ordinal
                || branch.DestinationBlockOrdinal > last))
                return Fail("Branching cleanup cannot leave or loop inside its finally.", out error);
            edges.AddRange(outgoing.Select(branch => new SemanticControlFlowEdge(
                ordinal, branch.DestinationBlockOrdinal, branch.Kind, "regular")));
        }
        HashSet<int> visited = new() { entry };
        for (int ordinal = entry; ordinal <= last; ++ordinal)
        {
            if (!visited.Contains(ordinal))
                return Fail("Branching cleanup contains an unreachable block.", out error);
            foreach (SemanticControlFlowEdge edge in edges.Where(edge =>
                edge.SourceBlockOrdinal == ordinal
                && edge.DestinationBlockOrdinal <= last))
                visited.Add(edge.DestinationBlockOrdinal);
        }
        if (flow.Branches.Count != edges.Count)
            return Fail("Branching cleanup has an unaccounted source edge.", out error);

        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.Ordinal == blocks.Count - 1 || block.IsReachable,
                block.ConditionKind, block.Operations,
                block.Ordinal == 1 ? null
                    : block.Ordinal == last ? ZeroPlaceholder(sites[0].Span)
                        : block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        localThrows = new[]
        {
            new CSharpLocalThrowSite(1, sites[0], BranchingCleanup:
                new CSharpBranchingCleanup(entry, last,
                    Enumerable.Range(entry, last - entry + 1).ToArray())),
        };
        return true;
    }

    private static bool TryBuildCatchFinally(
        SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out IReadOnlyList<CSharpNormalReturnCleanupSite> normalReturns,
        out IReadOnlyList<CSharpRethrowSite> rethrows,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        normalReturns = Array.Empty<CSharpNormalReturnCleanupSite>();
        rethrows = Array.Empty<CSharpRethrowSite>();
        error = null;
        SemanticThrowSite[] rethrowSites = flow.Throws.Where(site =>
            site.Kind == "rethrow").ToArray();
        bool hasRethrow = rethrowSites.Length == 1;
        bool nestedCatch = flow.Catches.Count == 2;
        bool hasLocalThrow = !nestedCatch && hasRethrow && flow.Throws.Count == 2
            && flow.Throws.Count(site => site.Kind == "throw") == 1;
        SemanticThrowSite? rethrow = hasRethrow ? rethrowSites[0] : null;
        int tryRegion = nestedCatch ? 6 : 4;
        int catchRegion = nestedCatch ? 7 : 5;
        int finallyRegion = nestedCatch ? 9 : 6;
        int cleanupEntry = nestedCatch ? 4 : 3;
        if (flow.Catches.Count is not (1 or 2) || nestedCatch && !hasRethrow
            || flow.Throws.Count != 0
                && !(hasRethrow && (flow.Throws.Count == 1 || hasLocalThrow))
            || flow.Regions.Count != (nestedCatch ? 10 : 7)
            || flow.Blocks is not { Count: >= 5 } blocks
            || flow.Branches.Count < 4
            || flow.Regions[0] is not { Kind: "root", ParentOrdinal: -1 }
            || flow.Regions[1] is not { Kind: "try_and_finally", ParentOrdinal: 0 }
            || flow.Regions[2] is not { Kind: "try", ParentOrdinal: 1 }
            || flow.Regions[3] is not { Kind: "try_and_catch", ParentOrdinal: 2 }
            || flow.Regions[4] is not { Kind: "try", ParentOrdinal: 3,
                FirstBlockOrdinal: 1 }
            || flow.Regions[4].LastBlockOrdinal != (nestedCatch ? 2 : 1)
            || nestedCatch && (flow.Regions[5] is not
                    { Kind: "try_and_catch", ParentOrdinal: 4,
                        FirstBlockOrdinal: 1, LastBlockOrdinal: 2 }
                || flow.Regions[6] is not { Kind: "try", ParentOrdinal: 5,
                    FirstBlockOrdinal: 1, LastBlockOrdinal: 1 }
                || flow.Regions[7] is not { Kind: "catch", ParentOrdinal: 5,
                    FirstBlockOrdinal: 2, LastBlockOrdinal: 2 }
                || flow.Regions[8] is not { Kind: "catch", ParentOrdinal: 3,
                    FirstBlockOrdinal: 3, LastBlockOrdinal: 3 })
            || !nestedCatch && flow.Regions[5] is not
                { Kind: "catch", ParentOrdinal: 3,
                    FirstBlockOrdinal: 2, LastBlockOrdinal: 2 }
            || flow.Regions[finallyRegion] is not { Kind: "finally", ParentOrdinal: 1 }
            || flow.Regions[finallyRegion].FirstBlockOrdinal != cleanupEntry
            || flow.Regions[finallyRegion].LastBlockOrdinal != blocks.Count - 2
            || flow.Regions[finallyRegion].LastBlockOrdinal > 18
            || flow.Catches[0] is not { HasFilter: false,
                ExceptionVariableSymbolId: null }
            || flow.Catches[0].RegionOrdinal != catchRegion
            || flow.Catches[0].ExceptionTypeId
                != CSharpThrowProducerLowerer.ExceptionTypeId
            || nestedCatch && (flow.Catches[1] is not
                    { RegionOrdinal: 8, HasFilter: false,
                        ExceptionVariableSymbolId: null }
                || flow.Catches[1].ExceptionTypeId
                    != CSharpThrowProducerLowerer.ExceptionTypeId)
            || blocks[0].Operations.Count != 0 || blocks[0].BranchValue is not null
            || blocks[^1].Operations.Count != 0 || blocks[^1].BranchValue is not null
            || blocks[1].EnclosingRegionOrdinal != tryRegion
            || blocks[2].EnclosingRegionOrdinal != catchRegion
            || nestedCatch && (blocks[3].EnclosingRegionOrdinal != 8
                || blocks[3].ConditionKind != "none"
                || blocks[3].Operations.Count != 0
                || blocks[3].BranchValue is not { } outerValue
                || !Supported(outerValue)
                || Descendants(outerValue).Any(operation => operation.Kind is
                    "await" or "throw" or "try" or "conditional" or "switch"
                        or "branch" or "loop"))
            || blocks[0].ConditionKind != "none"
            || blocks[1].ConditionKind != "none"
            || blocks[2].ConditionKind != "none"
            || blocks[^1].ConditionKind != "none"
            || blocks[1].Operations.Count != 0 || blocks[2].Operations.Count != 0
            || blocks[1].BranchValue is not { } tryValue || !Supported(tryValue)
            || hasLocalThrow && (tryValue.Kind != "object_creation"
                || tryValue.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                || tryValue.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
                || tryValue.Children.Count != 0
                || !flow.Throws.Any(site => site.Kind == "throw"
                    && site.ExceptionTypeId == CSharpThrowProducerLowerer.ExceptionTypeId
                    && site.Span.Start <= tryValue.Span.Start
                    && tryValue.Span.End <= site.Span.End))
            || hasRethrow && blocks[2].BranchValue is not null
            || !hasRethrow && (blocks[2].BranchValue is not { } catchValue
                || !Supported(catchValue))
            || new SemanticOperation?[] { tryValue, blocks[2].BranchValue }
                .Where(value => value is not null)
                .SelectMany(value => Descendants(value!))
                .Any(operation => operation.Kind is "await" or "throw" or "try"
                    or "conditional" or "switch" or "branch" or "loop")
            || hasRethrow && (rethrow is null
                || rethrow.Span.Start < flow.Catches[0].Span.Start
                || rethrow.Span.End > flow.Catches[0].Span.End)
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 0
                && branch.DestinationBlockOrdinal == 1
                && branch.Kind == "fallthrough"
                && branch.Semantics == "regular") != 1
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 1
                && branch.DestinationBlockOrdinal == (hasLocalThrow
                    ? -1 : blocks.Count - 1)
                && branch.Kind == "fallthrough"
                && branch.Semantics == (hasLocalThrow ? "throw" : "return")
                && branch.FinallyRegionOrdinals.SequenceEqual(hasLocalThrow
                    ? Array.Empty<int>() : new[] { finallyRegion })) != 1
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 2
                && branch.DestinationBlockOrdinal == (hasRethrow
                    ? -1 : blocks.Count - 1)
                && branch.Kind == "fallthrough"
                && branch.Semantics == (hasRethrow ? "rethrow" : "return")
                && branch.FinallyRegionOrdinals.SequenceEqual(hasRethrow
                    ? Array.Empty<int>() : new[] { finallyRegion })) != 1
            || nestedCatch && flow.Branches.Count(branch =>
                branch.SourceBlockOrdinal == 3
                && branch.DestinationBlockOrdinal == blocks.Count - 1
                && branch.Kind == "fallthrough"
                && branch.Semantics == "return"
                && branch.FinallyRegionOrdinals.SequenceEqual(
                    new[] { finallyRegion })) != 1
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal
                == flow.Regions[finallyRegion].LastBlockOrdinal
                && branch.DestinationBlockOrdinal == -1
                && branch.Kind == "fallthrough"
                && branch.Semantics == "structured_exception_handling") != 1)
            return Fail("Catch cleanup needs source-backed return or rethrow leaves and one bounded finally.",
                out error);

        if (hasRethrow && (!SemanticExceptionDispatchPlanner.TryBuild(flow,
                out var dispatch) || dispatch is null
            || dispatch.Routes[2].Steps.Count != (nestedCatch ? 2 : 1)
            || nestedCatch && (dispatch.Routes[1].Steps.Count != 3
                || dispatch.Routes[1].Steps[0].Kind != "catch"
                || !dispatch.Routes[1].Steps[0].HandlerOrdinals.SequenceEqual(
                    new[] { 0 })
                || dispatch.Routes[1].Steps[1].Kind != "catch"
                || !dispatch.Routes[1].Steps[1].HandlerOrdinals.SequenceEqual(
                    new[] { 1 })
                || dispatch.Routes[1].Steps[2] is not
                    { Kind: "finally", RegionOrdinal: 9 }
                || dispatch.Routes[2].Steps[0].Kind != "catch"
                || !dispatch.Routes[2].Steps[0].HandlerOrdinals.SequenceEqual(
                    new[] { 1 })
                || dispatch.Routes[3].Steps.Count != 1
                || dispatch.Routes[3].Steps[0] is not
                    { Kind: "finally", RegionOrdinal: 9 })
            || dispatch.Routes[2].Steps[^1] is not
                { Kind: "finally" } cleanupStep
            || cleanupStep.RegionOrdinal != finallyRegion
            || hasLocalThrow && (dispatch.Routes[1].Steps.Count != 2
                || dispatch.Routes[1].Steps[0].Kind != "catch"
                || !dispatch.Routes[1].Steps[0].HandlerOrdinals.SequenceEqual(
                    new[] { 0 })
                || dispatch.Routes[1].Steps[1] is not
                    { Kind: "finally", RegionOrdinal: 6 })))
            return Fail("Catch rethrow needs the source-derived outer finally route.",
                out error);

        int cleanupExit = flow.Regions[finallyRegion].LastBlockOrdinal;
        List<SemanticControlFlowEdge> edges = new()
        {
            new(0, 1, "fallthrough", "regular"),
            new(1, blocks.Count - 1, "fallthrough", "return"),
            new(2, blocks.Count - 1, "fallthrough", "return"),
        };
        if (nestedCatch)
            edges.Add(new(3, blocks.Count - 1, "fallthrough", "return"));
        HashSet<int> reachedCleanup = new() { cleanupEntry };
        for (int ordinal = cleanupEntry; ordinal <= cleanupExit; ++ordinal)
        {
            SemanticExceptionBlock block = blocks[ordinal];
            SemanticExceptionBranch[] outgoing = flow.Branches.Where(branch =>
                branch.SourceBlockOrdinal == ordinal).ToArray();
            if (!reachedCleanup.Contains(ordinal)
                || block.EnclosingRegionOrdinal != finallyRegion
                || block.Operations.Any(operation => !Supported(operation)
                    || Descendants(operation).Any(item => item.Kind is
                        "invocation" or "await" or "throw" or "try"
                        or "conditional" or "switch" or "branch" or "loop")))
                return Fail("Catch cleanup has an unsupported operation or block.", out error);
            if (ordinal == cleanupExit)
            {
                if (block.Operations.Count == 0 || block.ConditionKind != "none"
                    || block.BranchValue is not null || outgoing.Length != 1
                    || outgoing[0].DestinationBlockOrdinal != -1
                    || outgoing[0].Semantics != "structured_exception_handling")
                    return Fail("Catch cleanup needs one final exit.", out error);
                edges.Add(new(ordinal, blocks.Count - 1, "fallthrough", "return"));
                continue;
            }
            if (outgoing.Length == 2)
            {
                if (block.Operations.Count != 0 || block.ConditionKind == "none"
                    || block.BranchValue is not { } condition
                    || !PureThrowDecision(condition)
                    || outgoing.Count(branch => branch.Kind == "conditional") != 1
                    || outgoing.Count(branch => branch.Kind == "fallthrough") != 1
                    || outgoing.Select(branch => branch.DestinationBlockOrdinal)
                        .Distinct().Count() != 2)
                    return Fail("Catch cleanup needs pure branch decisions.", out error);
            }
            else if (outgoing.Length != 1 || block.ConditionKind != "none"
                || block.BranchValue is not null || outgoing[0].Kind != "fallthrough")
                return Fail("Catch cleanup needs bounded forward blocks.", out error);
            if (outgoing.Any(branch => branch.Semantics != "regular"
                || branch.DestinationBlockOrdinal <= ordinal
                || branch.DestinationBlockOrdinal > cleanupExit))
                return Fail("Catch cleanup cannot leave or loop.", out error);
            foreach (SemanticExceptionBranch branch in outgoing)
            {
                reachedCleanup.Add(branch.DestinationBlockOrdinal);
                edges.Add(new(ordinal, branch.DestinationBlockOrdinal,
                    branch.Kind, "regular"));
            }
        }
        if (flow.Branches.Count != edges.Count)
            return Fail("Catch cleanup has an unaccounted source edge.", out error);
        CSharpBranchingCleanup? branchingCleanup = cleanupExit == cleanupEntry ? null
            : new(cleanupEntry, cleanupExit,
                Enumerable.Range(cleanupEntry, cleanupExit - cleanupEntry + 1).ToArray());
        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.Ordinal >= cleanupEntry || block.IsReachable,
                block.ConditionKind, block.Operations,
                block.Ordinal == 1 && hasLocalThrow
                    ? ZeroPlaceholder(tryValue.Span)
                    : block.Ordinal == 2 && hasRethrow
                    ? ZeroPlaceholder(rethrow!.Span)
                    : block.Ordinal == cleanupExit
                    ? ZeroPlaceholder(blocks[cleanupExit].Operations[0].Span)
                    : block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        normalReturns = hasLocalThrow
            ? Array.Empty<CSharpNormalReturnCleanupSite>()
            : nestedCatch
            ? new[]
            {
                new CSharpNormalReturnCleanupSite(1, cleanupEntry, branchingCleanup),
                new CSharpNormalReturnCleanupSite(3, cleanupEntry, branchingCleanup),
            }
            : hasRethrow
            ? new[] { new CSharpNormalReturnCleanupSite(1, cleanupEntry, branchingCleanup) }
            : new[]
            {
                new CSharpNormalReturnCleanupSite(1, cleanupEntry, branchingCleanup),
                new CSharpNormalReturnCleanupSite(2, cleanupEntry, branchingCleanup),
            };
        if (hasRethrow)
            rethrows = new[]
            {
                new CSharpRethrowSite(2, 0, rethrow!,
                    nestedCatch ? null : cleanupEntry,
                    nestedCatch ? null : branchingCleanup),
            };
        if (hasLocalThrow)
            localThrows = new[]
            {
                new CSharpLocalThrowSite(1, flow.Throws.Single(site =>
                    site.Kind == "throw")),
            };
        return true;
    }

    private static bool TryBuildBranchingThrowFinally(
        SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out IReadOnlyList<CSharpNormalReturnCleanupSite> normalReturns,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        normalReturns = Array.Empty<CSharpNormalReturnCleanupSite>();
        error = null;
        if (flow.Catches.Count != 0 || flow.Throws.Count is < 1 or > 16
            || flow.Regions.Count != 4
            || flow.Regions[0].Kind != "root"
            || flow.Regions[1].Kind != "try_and_finally"
            || flow.Regions[1].ParentOrdinal != 0
            || flow.Regions[2].Kind != "try"
            || flow.Regions[2].ParentOrdinal != 1
            || flow.Regions[3].Kind != "finally"
            || flow.Regions[3].ParentOrdinal != 1
            || flow.Blocks is not { Count: >= 6 } blocks
            || !SemanticExceptionDispatchPlanner.TryBuild(flow, out var dispatch)
            || dispatch is null)
            return Fail("Mixed exits need one source-backed synchronous finally.",
                out error);

        SemanticExceptionRegion cleanupRegion = flow.Regions[3];
        int cleanupEntry = cleanupRegion.FirstBlockOrdinal;
        int cleanupExit = cleanupRegion.LastBlockOrdinal;
        if (cleanupEntry < 2 || cleanupExit != blocks.Count - 2
            || cleanupExit - cleanupEntry + 1 > 16
            || flow.Regions[2].FirstBlockOrdinal != 1
            || flow.Regions[2].LastBlockOrdinal != cleanupEntry - 1
            || blocks[0].Operations.Count != 0 || blocks[0].BranchValue is not null
            || blocks[^1].Operations.Count != 0 || blocks[^1].BranchValue is not null
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 0
                && branch.DestinationBlockOrdinal == 1
                && branch.Semantics == "regular") != 1)
            return Fail("Mixed exits have an unsupported cleanup region.",
                out error);

        CSharpBranchingCleanup? branchingCleanup = cleanupEntry == cleanupExit
            ? null : new(cleanupEntry, cleanupExit,
                Enumerable.Range(cleanupEntry, cleanupExit - cleanupEntry + 1).ToArray());
        List<SemanticControlFlowEdge> cleanupEdges = new();
        HashSet<int> reachedCleanup = new() { cleanupEntry };
        for (int ordinal = cleanupEntry; ordinal <= cleanupExit; ++ordinal)
        {
            SemanticExceptionBlock block = blocks[ordinal];
            SemanticExceptionBranch[] outgoing = flow.Branches.Where(branch =>
                branch.SourceBlockOrdinal == ordinal).ToArray();
            if (!reachedCleanup.Contains(ordinal)
                || block.EnclosingRegionOrdinal != cleanupRegion.Ordinal
                || block.Operations.Any(operation => !Supported(operation)
                    || Descendants(operation).Any(item => item.Kind is
                        "conditional" or "switch" or "branch" or "loop" or "await"
                        or "try" or "throw" or "invocation")))
                return Fail("Mixed exits have an unsupported cleanup operation.", out error);
            if (ordinal == cleanupExit)
            {
                if (block.Operations.Count == 0 || block.ConditionKind != "none"
                    || block.BranchValue is not null || outgoing.Length != 1
                    || outgoing[0].DestinationBlockOrdinal != -1
                    || outgoing[0].Semantics != "structured_exception_handling")
                    return Fail("Mixed exits need one final cleanup exit.", out error);
                cleanupEdges.Add(new(ordinal, blocks.Count - 1, "fallthrough", "return"));
                continue;
            }
            if (outgoing.Length == 2)
            {
                if (block.Operations.Count != 0 || block.ConditionKind == "none"
                    || block.BranchValue is not { } condition
                    || !PureThrowDecision(condition)
                    || outgoing.Count(branch => branch.Kind == "conditional") != 1
                    || outgoing.Count(branch => branch.Kind == "fallthrough") != 1
                    || outgoing.Select(branch => branch.DestinationBlockOrdinal)
                        .Distinct().Count() != 2)
                    return Fail("Mixed exits need pure cleanup decisions.", out error);
            }
            else if (outgoing.Length != 1 || block.ConditionKind != "none"
                || block.BranchValue is not null || outgoing[0].Kind != "fallthrough")
                return Fail("Mixed exits need bounded forward cleanup blocks.", out error);
            if (outgoing.Any(branch => branch.Semantics != "regular"
                || branch.DestinationBlockOrdinal <= ordinal
                || branch.DestinationBlockOrdinal > cleanupExit))
                return Fail("Mixed exit cleanup cannot leave or loop.", out error);
            foreach (SemanticExceptionBranch branch in outgoing)
            {
                reachedCleanup.Add(branch.DestinationBlockOrdinal);
                cleanupEdges.Add(new(ordinal, branch.DestinationBlockOrdinal,
                    branch.Kind, "regular"));
            }
        }

        List<CSharpLocalThrowSite> projected = new();
        List<CSharpNormalReturnCleanupSite> returns = new();
        List<SemanticControlFlowEdge> edges = new()
        {
            new(0, 1, "fallthrough", "regular"),
        };
        for (int ordinal = 1; ordinal < cleanupEntry; ++ordinal)
        {
            SemanticExceptionBlock block = blocks[ordinal];
            SemanticExceptionBranch[] outgoing = flow.Branches.Where(branch =>
                branch.SourceBlockOrdinal == ordinal).ToArray();
            if (block.EnclosingRegionOrdinal != flow.Regions[2].Ordinal)
                return Fail("A local throw branch escaped its validated try region.", out error);
            if (outgoing.Length == 1 && outgoing[0].Semantics == "throw")
            {
                SemanticOperation? expression = block.BranchValue;
                SemanticThrowSite[] sites = flow.Throws.Where(site =>
                    expression is not null
                    && site.Span.Start <= expression.Span.Start
                    && expression.Span.End <= site.Span.End).ToArray();
                if (outgoing[0].DestinationBlockOrdinal != -1
                    || block.Operations.Count != 0
                    || expression is not { Kind: "object_creation", IsSupported: true }
                    || expression.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                    || expression.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
                    || expression.Children.Count != 0 || sites.Length != 1
                    || sites[0].Kind != "throw"
                    || sites[0].ExceptionTypeId
                        != CSharpThrowProducerLowerer.ExceptionTypeId
                    || dispatch.Routes[ordinal].Steps.Count != 1
                    || dispatch.Routes[ordinal].Steps[0].Kind != "finally"
                    || dispatch.Routes[ordinal].Steps[0].RegionOrdinal
                        != cleanupRegion.Ordinal)
                    return Fail("Each local throw leaf needs one direct System.Exception.",
                        out error);
                projected.Add(new CSharpLocalThrowSite(ordinal, sites[0],
                    branchingCleanup is null ? new[] { cleanupEntry } : null,
                    BranchingCleanup: branchingCleanup));
                edges.Add(new(ordinal, cleanupEntry, "fallthrough", "regular"));
                continue;
            }
            if (outgoing.Length == 1 && outgoing[0].Semantics == "return")
            {
                SemanticExceptionBranch branch = outgoing[0];
                if (branch.DestinationBlockOrdinal != blocks.Count - 1
                    || branch.FinallyRegionOrdinals.Count != 1
                    || branch.FinallyRegionOrdinals[0] != cleanupRegion.Ordinal
                    || block.BranchValue is not { } value || !Supported(value)
                    || block.Operations.Any(operation => !Supported(operation))
                    || block.Operations.SelectMany(Descendants)
                        .Any(operation => operation.Kind is
                            "invocation" or "await" or "throw" or "try"
                            or "conditional" or "switch" or "branch" or "loop")
                    || Descendants(value).Any(operation => operation.Kind is
                        "await" or "throw" or "try" or "conditional" or "switch"
                        or "branch" or "loop"))
                    return Fail("A normal return needs one evaluated value before synchronous cleanup.",
                        out error);
                returns.Add(new(ordinal, cleanupEntry, branchingCleanup));
                edges.Add(new(ordinal, blocks.Count - 1, branch.Kind, "return"));
                continue;
            }
            if (outgoing.Length != 2 || block.Operations.Count != 0
                || block.ConditionKind == "none"
                || block.BranchValue is not { } condition
                || !BoundedTryDecision(condition)
                || outgoing.Any(branch => branch.Semantics != "regular"
                    || branch.DestinationBlockOrdinal <= ordinal
                    || branch.DestinationBlockOrdinal >= cleanupEntry)
                || outgoing.Select(branch => branch.DestinationBlockOrdinal)
                    .Distinct().Count() != 2
                || outgoing.Count(branch => branch.Kind == "fallthrough") != 1
                || outgoing.Count(branch => branch.Kind == "conditional") != 1)
                return Fail("The local throw decision tree needs pure forward branches.",
                    out error);
            edges.AddRange(outgoing.Select(branch => new SemanticControlFlowEdge(
                ordinal, branch.DestinationBlockOrdinal, branch.Kind, "regular")));
        }
        if (projected.Count != flow.Throws.Count
            || projected.Count + returns.Count > 16
            || projected.Select(site => site.Site.Span.Start).Distinct().Count()
                != projected.Count
            || flow.Branches.Count != edges.Count + cleanupEdges.Count
            || Enumerable.Range(1, cleanupEntry - 1).Any(ordinal =>
                edges.Count(edge => edge.DestinationBlockOrdinal == ordinal) != 1))
            return Fail("Every try path must reach a distinct throw or return leaf.",
                out error);
        edges.AddRange(cleanupEdges);
        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.Ordinal >= cleanupEntry || block.IsReachable,
                block.ConditionKind, block.Operations,
                projected.Any(site => site.BlockOrdinal == block.Ordinal)
                    ? null : block.Ordinal == cleanupExit
                        ? ZeroPlaceholder(projected[0].Site.Span) : block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        localThrows = projected;
        normalReturns = returns;
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
                new CSharpLocalThrowSite(throwOrdinal, site, new[] { cleanupOrdinal }),
                new CSharpLocalThrowSite(cleanupOrdinal, replacement!,
                    ReplacesThrowBlockOrdinal: throwOrdinal),
            }
            : new[] { new CSharpLocalThrowSite(throwOrdinal, site, new[] { cleanupOrdinal }) };
        return true;
    }

    private static bool TryBuildNestedDirectThrowFinally(
        SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph,
        out IReadOnlyList<CSharpLocalThrowSite> localThrows,
        out string? error)
    {
        graph = null;
        localThrows = Array.Empty<CSharpLocalThrowSite>();
        error = null;
        SemanticExceptionRegion[] regions = flow.Regions.ToArray();
        SemanticExceptionRegion[] finallyRegions = regions
            .Where(region => region.Kind == "finally").ToArray();
        int cleanupCount = finallyRegions.Length;
        if (flow.Catches.Count != 0 || flow.Throws.Count is not (1 or 2)
            || cleanupCount is < 2 or > 16
            || regions.Length != 1 + cleanupCount * 3
            || regions[0].Kind != "root"
            || flow.Blocks is not { } blocks || blocks.Count != cleanupCount + 3
            || flow.Branches.Count != cleanupCount + 2
            || !SemanticExceptionDispatchPlanner.TryBuild(flow, out var dispatch)
            || dispatch is null)
            return Fail("Nested throw cleanup needs one direct throw and bounded linear finally scopes.",
                out error);

        bool replacesError = flow.Throws.Count == 2;
        int[] replacementIndices = Enumerable.Range(0, cleanupCount)
            .Where(index => blocks[index + 2].BranchValue is not null).ToArray();
        if (replacesError && replacementIndices.Length != 1
            || !replacesError && replacementIndices.Length != 0)
            return Fail("Nested cleanup needs one source-backed replacement throw.",
                out error);
        int replacementIndex = replacesError ? replacementIndices[0] : -1;

        List<SemanticExceptionRegion> outerToInner = new(cleanupCount);
        int parentOrdinal = 0;
        int innermostTryOrdinal = -1;
        for (int index = 0; index < cleanupCount; ++index)
        {
            SemanticExceptionRegion[] pairs = regions.Where(region =>
                region.ParentOrdinal == parentOrdinal
                && region.Kind == "try_and_finally").ToArray();
            if (pairs.Length != 1)
                return Fail("Nested throw cleanup has an unsupported region tree.", out error);
            SemanticExceptionRegion[] tries = regions.Where(region =>
                region.ParentOrdinal == pairs[0].Ordinal && region.Kind == "try").ToArray();
            SemanticExceptionRegion[] cleanups = regions.Where(region =>
                region.ParentOrdinal == pairs[0].Ordinal && region.Kind == "finally").ToArray();
            if (tries.Length != 1 || cleanups.Length != 1)
                return Fail("Nested throw cleanup has an unsupported region tree.", out error);
            outerToInner.Add(cleanups[0]);
            parentOrdinal = tries[0].Ordinal;
            innermostTryOrdinal = tries[0].Ordinal;
        }
        SemanticExceptionRegion[] orderedCleanups = outerToInner
            .AsEnumerable().Reverse().ToArray();
        if (!orderedCleanups.Select(region => region.Ordinal)
                .OrderBy(ordinal => ordinal).SequenceEqual(
                    finallyRegions.Select(region => region.Ordinal)
                        .OrderBy(ordinal => ordinal))
            || blocks[0].Operations.Count != 0 || blocks[0].BranchValue is not null
            || blocks[^1].Operations.Count != 0 || blocks[^1].BranchValue is not null
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 0
                && branch.DestinationBlockOrdinal == 1
                && branch.Semantics == "regular") != 1
            || flow.Branches.Count(branch => branch.SourceBlockOrdinal == 1
                && branch.DestinationBlockOrdinal == -1
                && branch.Semantics == "throw") != 1
            || dispatch.Routes[1].Steps.Count != cleanupCount
            || dispatch.Routes[1].Steps.Where((step, index) =>
                step.Kind != "finally"
                || step.RegionOrdinal != orderedCleanups[index].Ordinal).Any())
            return Fail("Nested throw cleanup has an unsupported exceptional route.", out error);

        SemanticExceptionBlock throwBlock = blocks[1];
        SemanticOperation? expression = throwBlock.BranchValue;
        SemanticThrowSite[] originalSites = flow.Throws.Where(item =>
            expression is not null && item.Kind == "throw"
            && item.Span.Start <= expression.Span.Start
            && expression.Span.End <= item.Span.End).ToArray();
        SemanticThrowSite? site = originalSites.Length == 1 ? originalSites[0] : null;
        if (throwBlock.EnclosingRegionOrdinal != innermostTryOrdinal
            || throwBlock.Operations.Count != 0
            || expression is not { Kind: "object_creation", IsSupported: true }
            || expression.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
            || expression.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
            || expression.Children.Count != 0
            || site is null
            || site.ExceptionTypeId != CSharpThrowProducerLowerer.ExceptionTypeId)
            return Fail("Nested throw cleanup needs a direct zero-argument System.Exception.",
                out error);
        SemanticThrowSite? replacement = null;
        for (int index = 0; index < cleanupCount; ++index)
        {
            int ordinal = index + 2;
            SemanticExceptionRegion region = orderedCleanups[index];
            SemanticExceptionBlock block = blocks[ordinal];
            bool throwingCleanup = index == replacementIndex;
            SemanticOperation? replacementExpression = block.BranchValue;
            if (throwingCleanup)
            {
                SemanticThrowSite[] matches = flow.Throws.Where(item =>
                    item.Kind == "throw" && replacementExpression is not null
                    && item.Span.Start <= replacementExpression.Span.Start
                    && replacementExpression.Span.End <= item.Span.End).ToArray();
                if (matches.Length != 1 || matches[0].Span == site.Span
                    || matches[0].ExceptionTypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                    || replacementExpression is not { Kind: "object_creation", IsSupported: true }
                    || replacementExpression.TypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                    || replacementExpression.SymbolId != CSharpThrowProducerLowerer.ExceptionConstructorId
                    || replacementExpression.Children.Count != 0
                    || dispatch.Routes[ordinal].Steps.Count != cleanupCount - index - 1
                    || dispatch.Routes[ordinal].Steps.Where((step, routeIndex) =>
                        step.Kind != "finally" || step.RegionOrdinal
                            != orderedCleanups[index + routeIndex + 1].Ordinal).Any())
                    return Fail("A nested cleanup throw needs one new error before the outer finally.",
                        out error);
                replacement = matches[0];
            }
            if (region.FirstBlockOrdinal != ordinal || region.LastBlockOrdinal != ordinal
                || block.EnclosingRegionOrdinal != region.Ordinal
                || !throwingCleanup && block.Operations.Count == 0
                || !throwingCleanup && replacementExpression is not null
                || block.Operations.Any(operation => !Supported(operation)
                    || Descendants(operation).Any(item => item.Kind is
                        "conditional" or "switch" or "branch" or "loop" or "await"
                        or "try" or "throw" or "invocation"))
                || flow.Branches.Count(branch => branch.SourceBlockOrdinal == ordinal
                    && branch.DestinationBlockOrdinal == -1
                    && branch.Semantics == (throwingCleanup
                        ? "throw" : "structured_exception_handling")) != 1)
                return Fail("Nested throw cleanup needs one linear synchronous block per finally.",
                    out error);
        }
        if (flow.Throws.Count != (replacesError ? 2 : 1)
            || replacesError && replacement is null)
            return Fail("Nested cleanup did not account for every throw site.", out error);

        List<SemanticControlFlowEdge> edges = new()
        {
            new(0, 1, "fallthrough", "regular"),
        };
        for (int ordinal = 1; ordinal < blocks.Count - 2; ++ordinal)
            edges.Add(new(ordinal, ordinal + 1, "fallthrough", "regular"));
        edges.Add(new(blocks.Count - 2, blocks.Count - 1, "fallthrough", "return"));
        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.Ordinal == blocks.Count - 1 || block.IsReachable,
                block.ConditionKind, block.Operations,
                block.Ordinal == blocks.Count - 2 ? ZeroPlaceholder(site.Span)
                    : block.Ordinal == 1 || replacesError
                        && block.Ordinal == replacementIndex + 2 ? null
                        : block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        localThrows = replacesError
            ? new[]
            {
                new CSharpLocalThrowSite(1, site,
                    orderedCleanups.Select(region => region.FirstBlockOrdinal).ToArray()),
                new CSharpLocalThrowSite(replacementIndex + 2, replacement!,
                    orderedCleanups.Skip(replacementIndex + 1)
                        .Select(region => region.FirstBlockOrdinal).ToArray(),
                    ReplacesThrowBlockOrdinal: 1),
            }
            : new[]
            {
                new CSharpLocalThrowSite(1, site,
                    orderedCleanups.Select(region => region.FirstBlockOrdinal).ToArray()),
            };
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

    private static bool PureThrowDecision(SemanticOperation operation)
    {
        if (!operation.IsSupported || operation.IsChecked || operation.IsLifted
            || (operation.Kind is "binary" or "unary") && operation.SymbolId is not null)
            return false;
        bool allowed = operation.Kind switch
        {
            "binary" => operation.OperatorKind is
                ("equals" or "not_equals" or "less_than" or "less_than_or_equal"
                    or "greater_than" or "greater_than_or_equal" or "logical_and"
                    or "logical_or" or "bitwise_and" or "bitwise_or" or "bitwise_xor"),
            "unary" => operation.OperatorKind is "logical_not" or "bitwise_not",
            "field_reference" or "local_reference" or "parameter_reference"
                or "literal" => true,
            _ => false,
        };
        return allowed && operation.Children.All(PureThrowDecision);
    }

    // A try decision may update an int local or field before selecting a throw
    // leaf. The condition is emitted once by the CFG; checked/user operators and
    // calls stay excluded because their own failure would need another cleanup edge.
    private static bool BoundedTryDecision(SemanticOperation operation)
    {
        if (!operation.IsSupported || operation.IsChecked || operation.IsLifted
            || (operation.Kind is "binary" or "unary" or "increment_or_decrement")
                && operation.SymbolId is not null)
            return false;
        if (operation.Kind == "increment_or_decrement")
        {
            return operation.TypeId == "type:int32"
                && operation.OperatorKind is "increment" or "decrement"
                && operation.Children.Count == 1
                && operation.Children[0] is { IsSupported: true, TypeId: "type:int32",
                    Kind: "local_reference" or "field_reference" }
                && operation.Children[0].Children.Count == 0;
        }
        bool allowed = operation.Kind switch
        {
            "binary" => operation.OperatorKind is
                ("equals" or "not_equals" or "less_than" or "less_than_or_equal"
                    or "greater_than" or "greater_than_or_equal" or "logical_and"
                    or "logical_or" or "bitwise_and" or "bitwise_or" or "bitwise_xor"),
            "unary" => operation.OperatorKind is "logical_not" or "bitwise_not",
            "field_reference" or "local_reference" or "parameter_reference"
                or "literal" => true,
            _ => false,
        };
        return allowed && operation.Children.All(BoundedTryDecision);
    }

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

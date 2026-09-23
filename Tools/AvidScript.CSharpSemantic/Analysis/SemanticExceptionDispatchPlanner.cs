using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// A catch step tries handlers in source order and continues outward only if none match.
// The Guest still has to compare the runtime exception type and own the error root.
public sealed record SemanticExceptionDispatchStep(
    string Kind,
    int RegionOrdinal,
    IReadOnlyList<int> HandlerOrdinals);

public sealed record SemanticExceptionDispatchRoute(
    int SourceBlockOrdinal,
    IReadOnlyList<SemanticExceptionDispatchStep> Steps);

public sealed record SemanticExceptionDispatchPlan(
    IReadOnlyList<SemanticExceptionDispatchRoute> Routes);

public static class SemanticExceptionDispatchPlanner
{
    public static bool TryBuild(
        SemanticExceptionFlow flow,
        out SemanticExceptionDispatchPlan? plan)
    {
        plan = null;
        if (flow is null || flow.Regions is null || flow.Blocks is null
            || flow.Catches is null || flow.Regions.Count is 0 or > 512
            || flow.Blocks.Count is 0 or > 2048 || flow.Catches.Count > 256
            || flow.Catches.Any(handler => handler is null || handler.HasFilter)
            || flow.Regions[0] is null || flow.Regions[0].Kind != "root"
            || flow.Regions[0].LastBlockOrdinal != flow.Blocks.Count - 1)
            return false;

        IReadOnlyList<SemanticExceptionRegion> regions = flow.Regions;
        for (int ordinal = 0; ordinal < regions.Count; ++ordinal)
        {
            SemanticExceptionRegion? region = regions[ordinal];
            if (region is null || region.Ordinal != ordinal
                || (ordinal == 0 && region.ParentOrdinal != -1)
                || (ordinal > 0 && (region.ParentOrdinal < 0
                    || region.ParentOrdinal >= ordinal))
                || region.FirstBlockOrdinal < 0
                || region.LastBlockOrdinal < region.FirstBlockOrdinal
                || region.LastBlockOrdinal >= flow.Blocks.Count)
                return false;
        }
        HashSet<int> handlerRegions = new();
        for (int ordinal = 0; ordinal < flow.Catches.Count; ++ordinal)
        {
            SemanticCatchHandler handler = flow.Catches[ordinal];
            if (handler.Ordinal != ordinal || handler.RegionOrdinal < 0
                || handler.RegionOrdinal >= regions.Count
                || !handlerRegions.Add(handler.RegionOrdinal)
                || regions[handler.RegionOrdinal].Kind != "catch"
                || handler.ExceptionTypeId != regions[handler.RegionOrdinal].ExceptionTypeId
                    && !(handler.ExceptionTypeId is null
                        && regions[handler.RegionOrdinal].ExceptionTypeId == "type:object"))
                return false;
        }

        List<SemanticExceptionDispatchRoute> routes = new(flow.Blocks.Count);
        for (int blockOrdinal = 0; blockOrdinal < flow.Blocks.Count; ++blockOrdinal)
        {
            SemanticExceptionBlock? block = flow.Blocks[blockOrdinal];
            if (block is null || block.Ordinal != blockOrdinal
                || block.EnclosingRegionOrdinal < 0
                || block.EnclosingRegionOrdinal >= regions.Count
                || regions[block.EnclosingRegionOrdinal].FirstBlockOrdinal > blockOrdinal
                || regions[block.EnclosingRegionOrdinal].LastBlockOrdinal < blockOrdinal)
                return false;

            List<SemanticExceptionDispatchStep> steps = new();
            int childOrdinal = block.EnclosingRegionOrdinal;
            while (regions[childOrdinal].ParentOrdinal >= 0)
            {
                int parentOrdinal = regions[childOrdinal].ParentOrdinal;
                SemanticExceptionRegion parent = regions[parentOrdinal];
                SemanticExceptionRegion child = regions[childOrdinal];
                if (parent.Kind == "try_and_finally" && child.Kind == "try")
                {
                    int[] finallyRegions = regions
                        .Where(region => region.ParentOrdinal == parentOrdinal
                            && region.Kind == "finally")
                        .Select(region => region.Ordinal).ToArray();
                    if (finallyRegions.Length != 1) return false;
                    steps.Add(new("finally", finallyRegions[0], Array.Empty<int>()));
                }
                else if (parent.Kind == "try_and_catch" && child.Kind == "try")
                {
                    int[] handlers = flow.Catches
                        .Where(handler => FirstChildUnder(regions,
                            handler.RegionOrdinal, parentOrdinal) is { } handlerChild
                            && regions[handlerChild].Kind is ("catch" or "filter_and_handler"))
                        .OrderBy(handler => handler.Ordinal)
                        .Select(handler => handler.Ordinal)
                        .ToArray();
                    if (handlers.Length == 0) return false;
                    steps.Add(new("catch", parentOrdinal, handlers));
                }
                else if ((parent.Kind == "try_and_finally" && child.Kind != "finally")
                    || (parent.Kind == "try_and_catch"
                        && child.Kind is not ("catch" or "filter_and_handler")))
                {
                    return false;
                }
                childOrdinal = parentOrdinal;
            }
            routes.Add(new(blockOrdinal, steps));
        }
        plan = new(routes);
        return true;
    }

    private static int? FirstChildUnder(
        IReadOnlyList<SemanticExceptionRegion> regions,
        int descendantOrdinal,
        int ancestorOrdinal)
    {
        if (descendantOrdinal < 0 || descendantOrdinal >= regions.Count)
            return null;
        int current = descendantOrdinal;
        while (current > ancestorOrdinal)
        {
            int parent = regions[current].ParentOrdinal;
            if (parent == ancestorOrdinal) return current;
            if (parent < 0 || parent >= current) return null;
            current = parent;
        }
        return null;
    }
}

using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticExceptionFlowContractValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        ArgumentNullException.ThrowIfNull(document);
        if (document.ExceptionFlows is null)
            return document.SchemaVersion is not (32 or 33 or SemanticContract.ExceptionFlowSchemaVersion)
                && document.SemanticVersion is not ("1.41" or "1.42" or SemanticContract.ExceptionFlowSemanticVersion);

        IReadOnlyList<SemanticExceptionFlow> flows = document.ExceptionFlows;
        if (document.SchemaVersion != SemanticContract.ExceptionFlowSchemaVersion
            || document.SemanticVersion != SemanticContract.ExceptionFlowSemanticVersion
            || document.Succeeded || document.ControlFlowGraphs is null
            || flows.Count is 0 or > 256
            || document.Callables is null || document.Types is null)
            return false;

        HashSet<string> methods = document.Callables.Select(callable => callable.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> types = document.Types.Select(type => type.Id)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> seen = new(StringComparer.Ordinal);
        HashSet<string> graphMethods = new(StringComparer.Ordinal);
        foreach (SemanticControlFlowGraph? graph in document.ControlFlowGraphs)
        {
            if (graph is null || !methods.Contains(graph.MethodSymbolId)
                || !graphMethods.Add(graph.MethodSymbolId)
                || graph.Blocks is null || graph.Blocks.Count is 0 or > 2048
                || graph.EntryBlockOrdinal < 0 || graph.ExitBlockOrdinal < 0
                || graph.EntryBlockOrdinal >= graph.Blocks.Count
                || graph.ExitBlockOrdinal >= graph.Blocks.Count)
                return false;
        }
        foreach (SemanticExceptionFlow? flow in flows)
        {
            if (flow is null || string.IsNullOrWhiteSpace(flow.MethodSymbolId)
                || !methods.Contains(flow.MethodSymbolId) || !seen.Add(flow.MethodSymbolId)
                || graphMethods.Contains(flow.MethodSymbolId)
                || string.IsNullOrWhiteSpace(flow.SourceId) || flow.SourceLength < 0
                || flow.Regions is null || flow.Branches is null || flow.Blocks is null
                || flow.Throws is null || flow.Catches is null
                || flow.Regions.Count is 0 or > 512 || flow.Branches.Count > 4096
                || flow.Blocks.Count is 0 or > 2048
                || flow.Throws.Count > 256 || flow.Catches.Count > 256
                || flow.Throws.Count + flow.Catches.Count == 0)
                return false;

            for (int index = 0; index < flow.Regions.Count; ++index)
            {
                SemanticExceptionRegion? region = flow.Regions[index];
                if (region is null || region.Ordinal != index
                    || index == 0 && region.ParentOrdinal != -1
                    || index == 0 && region.Kind != "root"
                    || index > 0 && (region.ParentOrdinal < 0 || region.ParentOrdinal >= index)
                    || region.FirstBlockOrdinal < 0
                    || region.LastBlockOrdinal < region.FirstBlockOrdinal
                    || !KnownRegionKind(region.Kind)
                    || !KnownType(region.ExceptionTypeId, types))
                    return false;
            }

            int lastBlockOrdinal = flow.Regions[0].LastBlockOrdinal;
            if (flow.Blocks.Count != lastBlockOrdinal + 1)
                return false;
            int operationCount = 0;
            for (int index = 0; index < flow.Blocks.Count; ++index)
            {
                SemanticExceptionBlock? block = flow.Blocks[index];
                if (block is null || block.Ordinal != index
                    || block.Kind is not ("entry" or "block" or "exit")
                    || (index == 0) != (block.Kind == "entry")
                    || (index == lastBlockOrdinal) != (block.Kind == "exit")
                    || block.ConditionKind is not ("none" or "when_true" or "when_false")
                    || block.EnclosingRegionOrdinal < 0
                    || block.EnclosingRegionOrdinal >= flow.Regions.Count
                    || flow.Regions[block.EnclosingRegionOrdinal].FirstBlockOrdinal > index
                    || flow.Regions[block.EnclosingRegionOrdinal].LastBlockOrdinal < index
                    || block.Operations is null || block.Operations.Count > 1024)
                    return false;
                foreach (SemanticOperation? operation in block.Operations)
                    if (!ValidOperation(operation, flow.SourceLength, types, 0, ref operationCount))
                        return false;
                if (block.BranchValue is not null
                    && !ValidOperation(block.BranchValue, flow.SourceLength, types, 0, ref operationCount))
                    return false;
            }
            foreach (SemanticExceptionBranch? branch in flow.Branches)
            {
                if (branch is null || branch.SourceBlockOrdinal < 0
                    || branch.SourceBlockOrdinal > lastBlockOrdinal
                    || branch.DestinationBlockOrdinal < -1
                    || branch.DestinationBlockOrdinal > lastBlockOrdinal
                    || string.IsNullOrWhiteSpace(branch.Kind)
                    || branch.Kind is not ("fallthrough" or "conditional")
                    || !KnownBranchSemantics(branch.Semantics)
                    || !ValidRegions(branch.LeavingRegionOrdinals, flow.Regions.Count)
                    || !ValidRegions(branch.EnteringRegionOrdinals, flow.Regions.Count)
                    || !ValidRegions(branch.FinallyRegionOrdinals, flow.Regions.Count))
                    return false;
            }

            foreach (SemanticThrowSite? site in flow.Throws)
            {
                if (site is null || site.Kind is not ("throw" or "rethrow")
                    || site.Kind == "rethrow" && site.ExceptionTypeId is not null
                    || !KnownType(site.ExceptionTypeId, types)
                    || !ValidSpan(site.Span, flow.SourceLength))
                    return false;
            }

            for (int index = 0; index < flow.Catches.Count; ++index)
            {
                SemanticCatchHandler? handler = flow.Catches[index];
                if (handler is null || handler.Ordinal != index
                    || handler.RegionOrdinal < 0 || handler.RegionOrdinal >= flow.Regions.Count
                    || flow.Regions[handler.RegionOrdinal].Kind != "catch"
                    || handler.ExceptionTypeId != flow.Regions[handler.RegionOrdinal].ExceptionTypeId
                        && !(handler.ExceptionTypeId is null
                            && flow.Regions[handler.RegionOrdinal].ExceptionTypeId == "type:object")
                    || !KnownType(handler.ExceptionTypeId, types)
                    || !ValidSpan(handler.Span, flow.SourceLength))
                    return false;
            }
        }

        return true;
    }

    private static bool KnownType(string? typeId, IReadOnlySet<string> types) =>
        typeId is null || types.Contains(typeId);

    private static bool KnownRegionKind(string? kind) => kind is
        "root" or "local_lifetime" or "try" or "catch" or "finally" or
        "try_and_catch" or "try_and_finally" or "filter" or "filter_and_handler" or
        "static_local_initializer" or "erroneous_body";

    private static bool KnownBranchSemantics(string? semantics) => semantics is
        "none" or "regular" or "return" or "program_termination" or
        "structured_exception_handling" or "throw" or "rethrow" or "error";

    private static bool ValidRegions(IReadOnlyList<int>? ordinals, int count) =>
        ordinals is not null && ordinals.All(ordinal => ordinal >= 0 && ordinal < count)
            && ordinals.Distinct().Count() == ordinals.Count;

    private static bool ValidSpan(SemanticSpan? span, int sourceLength) =>
        span is not null && span.Start >= 0 && span.Length >= 0
            && (long)span.Start + span.Length <= sourceLength
            && span.Line >= 0 && span.Column >= 0
            && span.EndLine >= span.Line && span.EndColumn >= 0;

    private static bool ValidOperation(
        SemanticOperation? operation,
        int sourceLength,
        IReadOnlySet<string> types,
        int depth,
        ref int operationCount)
    {
        if (operation is null || ++operationCount > 8192 || depth > 64
            || string.IsNullOrWhiteSpace(operation.Kind)
            || !ValidSpan(operation.Span, sourceLength)
            || !KnownType(operation.TypeId, types)
            || operation.TypeArgumentIds is null
            || operation.TypeArgumentIds.Any(typeId => !types.Contains(typeId))
            || operation.Children is null || operation.Children.Count > 128)
            return false;
        foreach (SemanticOperation? child in operation.Children)
            if (!ValidOperation(child, sourceLength, types, depth + 1, ref operationCount))
                return false;
        return true;
    }
}

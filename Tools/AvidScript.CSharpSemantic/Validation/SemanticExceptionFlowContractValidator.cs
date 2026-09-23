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
            return document.SchemaVersion != SemanticContract.ExceptionFlowSchemaVersion
                && document.SemanticVersion != SemanticContract.ExceptionFlowSemanticVersion;

        IReadOnlyList<SemanticExceptionFlow> flows = document.ExceptionFlows;
        if (document.SchemaVersion != SemanticContract.ExceptionFlowSchemaVersion
            || document.SemanticVersion != SemanticContract.ExceptionFlowSemanticVersion
            || document.Succeeded || document.ControlFlowGraphs is not { Count: 0 }
            || flows.Count is 0 or > 256
            || document.Callables is null || document.Types is null)
            return false;

        HashSet<string> methods = document.Callables.Select(callable => callable.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> types = document.Types.Select(type => type.Id)
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> seen = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow? flow in flows)
        {
            if (flow is null || string.IsNullOrWhiteSpace(flow.MethodSymbolId)
                || !methods.Contains(flow.MethodSymbolId) || !seen.Add(flow.MethodSymbolId)
                || string.IsNullOrWhiteSpace(flow.SourceId) || flow.SourceLength < 0
                || flow.Regions is null || flow.Branches is null
                || flow.Throws is null || flow.Catches is null
                || flow.Regions.Count is 0 or > 512 || flow.Branches.Count > 4096
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
}

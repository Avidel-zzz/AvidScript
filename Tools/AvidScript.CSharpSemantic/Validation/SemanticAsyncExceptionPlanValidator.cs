using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Reader-side structural checks for the first source-backed async exception
// contract. Roslyn identity is established by the producer's region binder;
// this reader checks graph shape, handler identity and cancellation isolation.
public static class SemanticAsyncExceptionPlanValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document?.AsyncMethods is null || document.Source is null)
            return false;
        bool enabled = document.SchemaVersion == SemanticContract.AsyncExceptionFlowSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncExceptionFlowSemanticVersion;
        bool directCleanup = document.SchemaVersion == SemanticContract.DirectAwaitCleanupSchemaVersion
            && document.SemanticVersion == SemanticContract.DirectAwaitCleanupSemanticVersion;
        bool languageCancellation = document.SchemaVersion == SemanticContract.AsyncCancellationFlowSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncCancellationFlowSemanticVersion;
        bool localLifetime = SemanticContract.HasTaskLocalLifetimes(document);
        bool routedThrows = SemanticContract.HasAsyncThrowRouting(document);
        if (routedThrows && !document.AsyncMethods.Any(method => method?.Segments?.Any(segment =>
            segment?.Transfer?.Kind == SemanticAsyncMethod.RaiseExceptionTransferKind) == true)) return false;
        languageCancellation |= localLifetime && document.AsyncMethods.Any(method => method?.ExceptionPlan?.CancellationTypeId is not null);
        directCleanup |= localLifetime && !languageCancellation && document.AsyncMethods.Any(method => method?.ExceptionPlan is not null
            && method.Segments.Any(segment => segment?.AwaitSite?.ProducerKind is "delay" or "next_tick"
                && segment.Transfer?.CancellationTarget is >= 0));
        enabled |= directCleanup || languageCancellation || localLifetime;
        if (!enabled)
            return document.SchemaVersion != SemanticContract.AsyncExceptionFlowSchemaVersion
                && document.SemanticVersion != SemanticContract.AsyncExceptionFlowSemanticVersion
                && document.SchemaVersion != SemanticContract.DirectAwaitCleanupSchemaVersion
                && document.SemanticVersion != SemanticContract.DirectAwaitCleanupSemanticVersion
                && document.SchemaVersion != SemanticContract.AsyncCancellationFlowSchemaVersion
                && document.SemanticVersion != SemanticContract.AsyncCancellationFlowSemanticVersion
                && document.SchemaVersion != SemanticContract.AsyncThrowRoutingSchemaVersion
                && document.SemanticVersion != SemanticContract.AsyncThrowRoutingSemanticVersion
                && document.AsyncMethods.All(method => method is not null
                    && method.ExceptionPlan is null && method.Segments is not null
                    && method.Segments.All(segment => segment is not null
                        && (segment.Transfer is null
                            || segment.Transfer.CancellationTarget is null
                                && segment.Transfer.ExceptionTypeId is null
                                && !IsExceptionTransfer(segment.Transfer.Kind))));
        if (!localLifetime && !document.AsyncMethods.Any(method => method?.ExceptionPlan is not null))
            return false;
        if (directCleanup && !document.AsyncMethods.Any(method => method?.ExceptionPlan is not null
            && method.Segments.Any(segment => segment?.AwaitSite?.ProducerKind is "delay" or "next_tick"
                && segment.Transfer?.CancellationTarget is >= 0
                && segment.Transfer.SecondaryTarget == -1))) return false;
        foreach (SemanticAsyncMethod? method in document.AsyncMethods)
        {
            if (method?.Segments is null || method.Span is null) return false;
            if (method.ExceptionPlan is not { } plan)
            {
                if (method.Segments.Any(segment => segment is null
                    || segment.Transfer is { } transfer
                        && (transfer.CancellationTarget is not null
                            || transfer.ExceptionTypeId is not null
                            || IsExceptionTransfer(transfer.Kind)))) return false;
                continue;
            }
            if (method.TaskResultTypeId != "type:int32"
                || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
                || method.ExportName is not null
                || plan.SourceId != document.Source.SourceId
                || plan.SourceLength != document.Source.Length
                || plan.Regions is not { Count: > 0 and <= 64 }
                || plan.Catches is null || plan.Catches.Count > 32
                || method.Segments.Count is 0 or > SemanticAsyncMethod.MaximumControlFlowSegments
                || method.EntrySegmentOrdinal < 0
                || method.EntrySegmentOrdinal >= method.Segments.Count
                || method.Segments.Where((segment, ordinal) => segment is null
                    || segment.Ordinal != ordinal || segment.Transfer is null).Any())
                return false;
            if ((languageCancellation || localLifetime && plan.CancellationTypeId is not null)
                ? !SemanticAsyncCancellationPlanValidator.IsValid(document, method)
                : plan.CancellationTypeId is not null || plan.ExceptionScopes is not null)
                return false;
            HashSet<int> regionOrdinals = new();
            Dictionary<int, SemanticAsyncExceptionRegion> catchRegions = new();
            bool hasProtectedAwait = false;
            foreach (SemanticAsyncExceptionRegion? region in plan.Regions)
            {
                if (region is null || region.Kind is not ("try" or "catch" or "finally")
                    || region.RoslynRegionOrdinal < 0
                    || !regionOrdinals.Add(region.RoslynRegionOrdinal)
                    || region.SourceSpan is null
                    || region.SourceSpan.Start < method.Span.Start
                    || region.SourceSpan.Length <= 0
                    || (long)region.SourceSpan.Start + region.SourceSpan.Length
                        > (long)method.Span.Start + method.Span.Length
                    || region.Segments is null
                    || !region.Segments.SequenceEqual(region.Segments.Distinct().Order())
                    || region.Segments.Any(ordinal => ordinal < 0
                        || ordinal >= method.Segments.Count)) return false;
                if (region.Kind == "catch")
                    catchRegions.Add(region.RoslynRegionOrdinal, region);
                if (region.Kind == "try")
                    hasProtectedAwait |= region.Segments.Any(ordinal =>
                        method.Segments[ordinal].AwaitSite is { ProducerKind: "task_call" or "task_local" }
                        || (directCleanup || languageCancellation || localLifetime) && method.Segments[ordinal].AwaitSite is
                            { ProducerKind: "delay" or "next_tick" }
                        || routedThrows && method.Segments[ordinal].Transfer?.Kind
                            == SemanticAsyncMethod.RaiseExceptionTransferKind);
            }
            if (!hasProtectedAwait || !plan.Regions.Any(region => region.Kind == "try")
                || !plan.Regions.Any(region => region.Kind is "catch" or "finally")
                || catchRegions.Count != plan.Catches.Count
                || plan.Catches.Any(handler => handler is null || handler.HasFilter
                    || handler.ExceptionVariableSymbolId is not null
                    || !catchRegions.TryGetValue(handler.RegionOrdinal,
                        out SemanticAsyncExceptionRegion? region)
                    || handler.Span != region.SourceSpan)) return false;
            // A throw in finally is outside its sibling catch. Only producers
            // within protected try regions can make a pruned catch necessary.
            int executableCatchCount = plan.Catches.Count(handler =>
                catchRegions[handler.RegionOrdinal].Segments.Count > 0);
            if (executableCatchCount != plan.Catches.Count
                && (!directCleanup || method.Segments.Any(segment =>
                    segment.AwaitSite?.ProducerKind is "task_call" or "task_local"
                    || segment.Transfer?.Kind == SemanticAsyncMethod.ThrowTransferKind
                        && plan.Regions.Any(region => region.Kind == "try"
                            && region.Segments.Contains(segment.Ordinal)))))
                return false;
            HashSet<int> catchDecisionSegments = new();
            foreach (SemanticAsyncSegment segment in method.Segments)
            {
                SemanticAsyncControlTransfer transfer = segment.Transfer!;
                bool validTargets = transfer.PrimaryTarget >= -1
                    && transfer.PrimaryTarget < method.Segments.Count
                    && transfer.SecondaryTarget >= -1
                    && transfer.SecondaryTarget < method.Segments.Count
                    && (transfer.CancellationTarget is null
                        || transfer.CancellationTarget >= 0
                            && transfer.CancellationTarget < method.Segments.Count);
                if (!validTargets) return false;
                if (transfer.Kind != SemanticAsyncMethod.AwaitTransferKind
                        && transfer.CancellationTarget is not null
                    || transfer.Kind != SemanticAsyncMethod.CatchMatchTransferKind
                        && transfer.ExceptionTypeId is not null) return false;
                switch (transfer.Kind)
                {
                    case SemanticAsyncMethod.AwaitTransferKind:
                        bool taskAwait = segment.AwaitSite?.ProducerKind is "task_call" or "task_local";
                        bool directAwait = (directCleanup || languageCancellation)
                            && segment.AwaitSite?.ProducerKind is "delay" or "next_tick";
                        bool protectedAwait = plan.Regions.Any(region => region.Kind == "try"
                            && region.Segments.Contains(segment.Ordinal));
                        if (segment.AwaitSite is null || transfer.PrimaryTarget < 0
                            || transfer.ExceptionTypeId is not null
                            || taskAwait && ((transfer.SecondaryTarget >= 0) !=
                                (transfer.CancellationTarget is >= 0)
                                || (languageCancellation
                                    ? transfer.SecondaryTarget >= 0
                                        && transfer.SecondaryTarget != transfer.CancellationTarget
                                    : transfer.SecondaryTarget == transfer.CancellationTarget))
                            || directAwait && transfer.SecondaryTarget >= 0
                            || !taskAwait && !directAwait && transfer.CancellationTarget is not null)
                            return false;
                        if (protectedAwait && (taskAwait && transfer.SecondaryTarget < 0
                            || directAwait && transfer.CancellationTarget is not >= 0
                            || !taskAwait && !directAwait)) return false;
                        if (directAwait && transfer.CancellationTarget == transfer.PrimaryTarget)
                            return false;
                        if (transfer.SecondaryTarget >= 0
                            && !protectedAwait || transfer.CancellationTarget is not null
                            && !protectedAwait) return false;
                        if (!languageCancellation && directAwait && transfer.CancellationTarget is int directCancellation
                            && !plan.Regions.Any(region => region.Kind == "finally"
                                && region.Segments.Contains(directCancellation))) return false;
                        if (!languageCancellation && transfer.CancellationTarget is int cancellationTarget
                            && !ValidCancellationPath(method, cancellationTarget)) return false;
                        break;
                    case SemanticAsyncMethod.CatchMatchTransferKind:
                        if (segment.AwaitSite is not null || transfer.Condition is not null
                            || transfer.PrimaryTarget < 0 || transfer.SecondaryTarget < 0
                            || !plan.Catches.Any(handler => handler.ExceptionTypeId
                                == transfer.ExceptionTypeId
                                && catchRegions[handler.RegionOrdinal].Segments
                                    .Contains(transfer.PrimaryTarget))
                            || !catchDecisionSegments.Add(segment.Ordinal)) return false;
                        break;
                    case SemanticAsyncMethod.PropagateFaultTransferKind:
                    case SemanticAsyncMethod.PropagateCancellationTransferKind:
                        if (languageCancellation) return false;
                        goto case SemanticAsyncMethod.PropagateExceptionTransferKind;
                    case SemanticAsyncMethod.PropagateExceptionTransferKind:
                        if (transfer.Kind == SemanticAsyncMethod.PropagateExceptionTransferKind
                            && !languageCancellation) return false;
                        if (segment.AwaitSite is not null || transfer.Condition is not null
                            || transfer.PrimaryTarget != -1 || transfer.SecondaryTarget != -1
                            || transfer.CancellationTarget is not null
                            || transfer.ExceptionTypeId is not null) return false;
                        break;
                    case SemanticAsyncMethod.GotoTransferKind:
                    case SemanticAsyncMethod.RethrowTransferKind:
                    case SemanticAsyncMethod.EndCatchTransferKind:
                        if (transfer.Kind != SemanticAsyncMethod.GotoTransferKind
                            && !languageCancellation) return false;
                        if (segment.AwaitSite is not null || transfer.Condition is not null
                            || transfer.PrimaryTarget < 0 || transfer.SecondaryTarget != -1)
                            return false;
                        break;
                    case SemanticAsyncMethod.BranchTransferKind:
                        if (segment.AwaitSite is not null
                            || transfer.Condition?.TypeId != "type:bool"
                            || transfer.PrimaryTarget < 0 || transfer.SecondaryTarget < 0)
                            return false;
                        break;
                    case SemanticAsyncMethod.ReturnTransferKind:
                        if (segment.AwaitSite is not null || transfer.Condition?.TypeId
                                != method.TaskResultTypeId
                            || transfer.PrimaryTarget != -1 || transfer.SecondaryTarget != -1)
                            return false;
                        break;
                    case SemanticAsyncMethod.ThrowTransferKind:
                        if (segment.AwaitSite is not null || transfer.Condition is not
                            { Kind: "object_creation", IsSupported: true }
                            || transfer.PrimaryTarget != -1 || transfer.SecondaryTarget != -1)
                            return false;
                        break;
                    case SemanticAsyncMethod.RaiseExceptionTransferKind:
                        if (!routedThrows || !languageCancellation || segment.AwaitSite is not null
                            || transfer.Condition is not { Kind: "object_creation", IsSupported: true }
                            || transfer.PrimaryTarget < 0 || transfer.SecondaryTarget != -1) return false;
                        break;
                    default:
                        return false;
                }
            }
            if (executableCatchCount != catchDecisionSegments.Count
                || !SemanticAsyncExceptionOwnerFlow.TryAnalyze(method, out _)) return false;
        }
        return true;
    }

    private static bool ValidCancellationPath(SemanticAsyncMethod method,
        int entry)
    {
        Stack<int> pending = new();
        HashSet<int> visited = new();
        bool hasTerminal = false;
        pending.Push(entry);
        while (pending.TryPop(out int ordinal))
        {
            if (ordinal < 0 || ordinal >= method.Segments.Count) return false;
            if (!visited.Add(ordinal)) continue;
            SemanticAsyncControlTransfer transfer = method.Segments[ordinal].Transfer!;
            switch (transfer.Kind)
            {
                case SemanticAsyncMethod.GotoTransferKind:
                    pending.Push(transfer.PrimaryTarget);
                    break;
                case SemanticAsyncMethod.BranchTransferKind:
                    pending.Push(transfer.PrimaryTarget);
                    pending.Push(transfer.SecondaryTarget);
                    break;
                case SemanticAsyncMethod.PropagateCancellationTransferKind:
                case SemanticAsyncMethod.ThrowTransferKind:
                    hasTerminal = true;
                    break;
                default:
                    return false;
            }
        }
        return hasTerminal;
    }

    private static bool IsExceptionTransfer(string kind) =>
        kind is SemanticAsyncMethod.CatchMatchTransferKind
            or SemanticAsyncMethod.PropagateFaultTransferKind
            or SemanticAsyncMethod.PropagateCancellationTransferKind
            or SemanticAsyncMethod.PropagateExceptionTransferKind
            or SemanticAsyncMethod.RethrowTransferKind
            or SemanticAsyncMethod.EndCatchTransferKind
            or SemanticAsyncMethod.RaiseExceptionTransferKind;
}

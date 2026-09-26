using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Schema 44 shares exception dispatch between language faults and cancellation,
// but preserves their distinct owner/terminal states. Scope identities come from
// the Roslyn binder; this reader independently checks every dispatch/unwind edge.
public static class SemanticAsyncCancellationPlanValidator
{
    public const string CancellationTypeId = "type:global::System.Threading.Tasks.TaskCanceledException";

    public static bool IsValid(SemanticDocument document, SemanticAsyncMethod method)
    {
        if (document is null || method is null
            || document.SchemaVersion != SemanticContract.AsyncCancellationFlowSchemaVersion
            || document.SemanticVersion != SemanticContract.AsyncCancellationFlowSemanticVersion
            || method.ExceptionPlan is not { } plan
            || plan.CancellationTypeId != CancellationTypeId
            || plan.ExceptionScopes is not { Count: > 0 and <= 64 } scopes
            || plan.Regions is null || plan.Catches is null || method.Segments is null
            || plan.Regions.Any(region => region is null || region.SourceSpan is null
                || region.Segments is null)
            || plan.Catches.Any(handler => handler is null)
            || plan.Catches.Select(handler => handler.RegionOrdinal).Distinct().Count() != plan.Catches.Count
            || plan.Catches.Select(handler => handler.Ordinal).Distinct().Count() != plan.Catches.Count
            || method.Segments.Any(segment => segment?.Transfer is null)
            || !SemanticClassContractValidator.IsValid(document)) return false;
        Dictionary<string, string> bases = new(StringComparer.Ordinal)
        {
            [CancellationTypeId] = "type:global::System.OperationCanceledException",
            ["type:global::System.OperationCanceledException"] = "type:global::System.SystemException",
            ["type:global::System.SystemException"] = "type:global::System.Exception",
            ["type:global::System.Exception"] = "type:object",
        };
        if (bases.Any(pair => document.ClassTypes.SingleOrDefault(type => type.TypeId == pair.Key)
            ?.BaseTypeId != pair.Value)) return false;
        if (plan.Regions.Select(region => region.RoslynRegionOrdinal).Distinct().Count()
                != plan.Regions.Count
            || scopes.Any(scope => scope is null || scope.CatchRegionOrdinals is null)
            || !scopes.Select(scope => scope.ProtectedRegionOrdinal).SequenceEqual(
                plan.Regions.Where(region => region.Kind == "try")
                    .Select(region => region.RoslynRegionOrdinal).Order())
            || !scopes.SelectMany(scope => scope.CatchRegionOrdinals).Order().SequenceEqual(
                plan.Regions.Where(region => region.Kind == "catch")
                    .Select(region => region.RoslynRegionOrdinal).Order())
            || !scopes.Where(scope => scope.FinallyRegionOrdinal is not null)
                .Select(scope => scope.FinallyRegionOrdinal!.Value).Order().SequenceEqual(
                    plan.Regions.Where(region => region.Kind == "finally")
                        .Select(region => region.RoslynRegionOrdinal).Order())) return false;

        Dictionary<int, SemanticAsyncExceptionRegion> regions = plan.Regions
            .ToDictionary(region => region.RoslynRegionOrdinal);
        bool ValidTarget(int target) => target >= 0 && target < method.Segments.Count;
        foreach (SemanticAsyncExceptionScope scope in scopes)
        {
            if (!ValidTarget(scope.DispatchTarget) || !ValidTarget(scope.UnwindTarget)
                || !regions.TryGetValue(scope.ProtectedRegionOrdinal, out var protectedRegion)) return false;
            var parent = scopes.Where(candidate => candidate != scope
                && Contains(regions[candidate.ProtectedRegionOrdinal].SourceSpan, protectedRegion.SourceSpan))
                .OrderBy(candidate => regions[candidate.ProtectedRegionOrdinal].SourceSpan.Length)
                .FirstOrDefault();
            if (scope.ParentProtectedRegionOrdinal != parent?.ProtectedRegionOrdinal
                || parent is not null && protectedRegion.Segments.Any(ordinal =>
                    !regions[parent.ProtectedRegionOrdinal].Segments.Contains(ordinal))) return false;
            int[] orderedCatches = plan.Catches.Where(handler =>
                    scope.CatchRegionOrdinals.Contains(handler.RegionOrdinal))
                .OrderBy(handler => handler.Ordinal).Select(handler => handler.RegionOrdinal).ToArray();
            if (!orderedCatches.SequenceEqual(scope.CatchRegionOrdinals)) return false;
            int cursor = scope.DispatchTarget;
            foreach (int catchRegion in scope.CatchRegionOrdinals)
            {
                if (!ValidTarget(cursor)) return false;
                SemanticAsyncControlTransfer transfer = method.Segments[cursor].Transfer!;
                SemanticCatchHandler? handler = plan.Catches.SingleOrDefault(item => item.RegionOrdinal == catchRegion);
                if (handler is null || transfer.Kind != SemanticAsyncMethod.CatchMatchTransferKind
                    || transfer.ExceptionTypeId != handler.ExceptionTypeId
                    || !regions[catchRegion].Segments.Contains(transfer.PrimaryTarget)) return false;
                cursor = transfer.SecondaryTarget;
            }
            if (cursor != scope.UnwindTarget
                || !ValidUnwind(method, scope, parent?.DispatchTarget, regions)) return false;
        }
        foreach (SemanticAsyncSegment segment in method.Segments)
        {
            var transfer = segment.Transfer!;
            if (transfer.Kind == SemanticAsyncMethod.ThrowTransferKind) return false;
            if (segment.AwaitSite is not null)
            {
                var scope = scopes.Where(candidate => regions[candidate.ProtectedRegionOrdinal]
                        .Segments.Contains(segment.Ordinal))
                    .OrderBy(candidate => regions[candidate.ProtectedRegionOrdinal].SourceSpan.Length)
                    .FirstOrDefault();
                if (scope is not null && (transfer.CancellationTarget != scope.DispatchTarget
                    || (segment.AwaitSite.ProducerKind is "task_call" or "task_local")
                        && transfer.SecondaryTarget != scope.DispatchTarget)) return false;
            }
            if (transfer.Kind == SemanticAsyncMethod.RethrowTransferKind)
            {
                var owner = scopes.SingleOrDefault(scope => scope.CatchRegionOrdinals.Any(region =>
                    regions[region].Segments.Contains(segment.Ordinal)));
                if (owner is null || transfer.PrimaryTarget != owner.UnwindTarget) return false;
            }
        }
        return true;
    }

    private static bool Contains(SemanticSpan outer, SemanticSpan inner) =>
        outer.Start <= inner.Start && (long)outer.Start + outer.Length >= (long)inner.Start + inner.Length;

    private static bool ValidUnwind(SemanticAsyncMethod method, SemanticAsyncExceptionScope scope,
        int? parentDispatch, IReadOnlyDictionary<int, SemanticAsyncExceptionRegion> regions)
    {
        IReadOnlyList<int> cleanup = scope.FinallyRegionOrdinal is int finallyRegion
            ? regions[finallyRegion].Segments : Array.Empty<int>();
        if (scope.FinallyRegionOrdinal is not null && !cleanup.Contains(scope.UnwindTarget)) return false;
        Stack<int> pending = new();
        HashSet<int> visited = new();
        bool hasExit = false;
        pending.Push(scope.UnwindTarget);
        while (pending.TryPop(out int ordinal))
        {
            if (ordinal < 0 || ordinal >= method.Segments.Count) return false;
            if (!visited.Add(ordinal)) continue;
            var transfer = method.Segments[ordinal].Transfer!;
            if (parentDispatch == ordinal || parentDispatch is null
                && transfer.Kind == SemanticAsyncMethod.PropagateExceptionTransferKind)
            {
                hasExit = true;
                continue;
            }
            if (!cleanup.Contains(ordinal)) return false;
            if (transfer.Kind == SemanticAsyncMethod.GotoTransferKind)
                pending.Push(transfer.PrimaryTarget);
            else if (transfer.Kind == SemanticAsyncMethod.BranchTransferKind)
            {
                pending.Push(transfer.PrimaryTarget);
                pending.Push(transfer.SecondaryTarget);
            }
            else return false;
        }
        return hasExit;
    }
}

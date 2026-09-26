using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Checks routes against source-owned region membership. It never recovers a
// cloned cleanup's identity from the span of another clone.
public static class SemanticAsyncSynchronousExceptionValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document?.AsyncMethods is null
            || document.AsyncMethods.Any(method => method?.Segments is null
                || method.Segments.Any(segment => segment is null))) return false;
        if (!SemanticContract.HasAsyncSynchronousExceptions(document))
            return document.SchemaVersion != SemanticContract.AsyncSynchronousExceptionSchemaVersion
                && document.SemanticVersion != SemanticContract.AsyncSynchronousExceptionSemanticVersion
                && document.AsyncMethods.All(method => method.Segments.All(segment =>
                    segment.SynchronousExceptionTarget is null));
        if (document.StaticInitialization is not null
            || !document.AsyncMethods.Any(method => method.TaskResultTypeId == "type:int32")) return false;
        foreach (var method in document.AsyncMethods)
        {
            if (method.TaskResultTypeId is null)
            {
                if (method.Segments.Any(segment => segment.SynchronousExceptionTarget is not null)) return false;
                continue;
            }
            if (method.TaskResultTypeId != "type:int32"
                || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
                || method.Segments.Count is 0 or > SemanticAsyncMethod.MaximumControlFlowSegments
                || method.ExceptionPlan is not { Regions: { Count: <= 64 } regions, ExceptionScopes: { Count: <= 64 } scopes }
                || regions.Any(region => region?.Segments is null || region.SourceSpan is null)
                || scopes.Any(scope => scope?.CatchRegionOrdinals is null)
                || scopes.Select(scope => scope.ProtectedRegionOrdinal).Distinct().Count() != scopes.Count
                || regions.Select(region => region.RoslynRegionOrdinal).Distinct().Count() != regions.Count)
                return false;
            foreach (var segment in method.Segments)
            {
                if (segment.Statements is null || segment.Transfer is null || segment.Span is null
                    || segment.Statements.Any(statement => statement is null || !ValidOperation(statement.Operation))
                    || segment.Transfer.Condition is { } condition && !ValidOperation(condition)
                    || segment.AwaitSite is { } site && (site.Arguments is null
                        || site.Arguments.Any(argument => !ValidOperation(argument))
                        || site.CancellationToken is { } token && !ValidOperation(token))) return false;
                bool required = SemanticAsyncSynchronousExceptions.RequiresRoute(segment);
                if (required != segment.SynchronousExceptionTarget.HasValue) return false;
                if (!required) continue;
                if (segment.SynchronousExceptionTarget is not int target || target < 0
                    || target >= method.Segments.Count || target == segment.Ordinal) return false;
                // Source-bearing evaluation segments must not lose their bound
                // membership. This is a consistency check, not route inference.
                if (regions.Any(region => Contains(region.SourceSpan, segment.Span)
                    != region.Segments.Contains(segment.Ordinal))) return false;
                var region = regions.Where(item => item.Segments.Contains(segment.Ordinal))
                    .OrderBy(item => item.SourceSpan.Length).FirstOrDefault();
                var owners = region is null ? Array.Empty<SemanticAsyncExceptionScope>()
                    : scopes.Where(scope => scope.ProtectedRegionOrdinal == region.RoslynRegionOrdinal
                        || scope.CatchRegionOrdinals.Contains(region.RoslynRegionOrdinal)
                        || scope.FinallyRegionOrdinal == region.RoslynRegionOrdinal).ToArray();
                if (region is not null && owners.Length != 1) return false;
                var owner = owners.FirstOrDefault();
                int? expected = region?.Kind switch
                {
                    "try" => owner!.DispatchTarget,
                    "catch" => owner!.UnwindTarget,
                    "finally" => owner!.ParentProtectedRegionOrdinal is int parent
                        ? scopes.SingleOrDefault(scope => scope.ProtectedRegionOrdinal == parent)?.DispatchTarget
                        : null,
                    null => null,
                    _ => -1,
                };
                if (owner?.ParentProtectedRegionOrdinal is int parentOrdinal
                    && scopes.Count(scope => scope.ProtectedRegionOrdinal == parentOrdinal) != 1) return false;
                if (expected is int expectedTarget ? target != expectedTarget
                    : method.Segments[target].Transfer?.Kind != SemanticAsyncMethod.PropagateExceptionTransferKind)
                    return false;
            }
            if (method.CompilerLocals is null || method.CompilerLocals.Any(local => local is null
                || local.SymbolId is null || local.Span is null
                || local.SymbolId.StartsWith(SemanticAsyncCleanupLocals.Prefix(method.MethodSymbolId), StringComparison.Ordinal)
                    && !SemanticAsyncCleanupLocals.IsValid(document, method, local))) return false;
        }
        return true;
    }

    private static bool ValidOperation(SemanticOperation? operation, int depth = 0) =>
        operation?.Children is not null && depth < SemanticAsyncMethod.MaximumStructuredFlowNodes
        && operation.Children.All(child => ValidOperation(child, depth + 1));

    private static bool Contains(SemanticSpan outer, SemanticSpan inner) =>
        outer.Start <= inner.Start && (long)outer.Start + outer.Length >= (long)inner.Start + inner.Length;
}

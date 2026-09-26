using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticAsyncErrorPlanValidator
{
    private static readonly IReadOnlyDictionary<string, string> Constructors =
        new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["type:global::System.Exception"] =
                "symbol:method:global::System.Exception..ctor():void",
            ["type:global::System.InvalidOperationException"] =
                "symbol:method:global::System.InvalidOperationException..ctor():void",
            ["type:global::System.ArgumentException"] =
                "symbol:method:global::System.ArgumentException..ctor():void",
        };

    public static bool TryGetConstructor(string? exceptionTypeId, out string? constructorSymbolId) =>
        Constructors.TryGetValue(exceptionTypeId ?? string.Empty, out constructorSymbolId);

    public static bool IsValid(SemanticDocument document)
    {
        if (document is null || document.AsyncMethods is null || document.Source is null
            || document.Types is null) return false;
        bool originalContract = document.SchemaVersion == SemanticContract.AsyncLanguageErrorSchemaVersion
            && document.SemanticVersion == SemanticContract.AsyncLanguageErrorSemanticVersion;
        bool enabled = originalContract || SemanticContract.HasTaskLocalLifetimes(document)
            || document.SchemaVersion == SemanticContract.AsyncExceptionFlowSchemaVersion
                && document.SemanticVersion == SemanticContract.AsyncExceptionFlowSemanticVersion
            || document.SchemaVersion == SemanticContract.DirectAwaitCleanupSchemaVersion
                && document.SemanticVersion == SemanticContract.DirectAwaitCleanupSemanticVersion
            || document.SchemaVersion == SemanticContract.AsyncCancellationFlowSchemaVersion
                && document.SemanticVersion == SemanticContract.AsyncCancellationFlowSemanticVersion;
        if (originalContract && !document.AsyncMethods.Any(method => method?.ErrorPlan is not null))
            return false;
        foreach (SemanticAsyncMethod? method in document.AsyncMethods)
        {
            if (method is null || method.Segments is null) return false;
            SemanticAsyncSegment[] throwSegments = method.Segments.Where(segment =>
                segment?.Transfer?.Kind == SemanticAsyncMethod.ThrowTransferKind).ToArray();
            if (!enabled)
            {
                if (method.ErrorPlan is not null || throwSegments.Length != 0) return false;
                continue;
            }
            if (method.ErrorPlan is not { } plan)
            {
                if (throwSegments.Length != 0) return false;
                continue;
            }
            if (method.Span is null
                || method.Segments.Count is 0 or > SemanticAsyncMethod.MaximumControlFlowSegments
                || method.Segments.Where((segment, ordinal) =>
                    segment is null || segment.Ordinal != ordinal || segment.Transfer is null).Any())
                return false;
            if (method.TaskResultTypeId != "type:int32" || method.ExportName is not null
                || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
                || plan.SourceId != document.Source.SourceId
                || plan.SourceLength != document.Source.Length
                || plan.SourceLength < 0
                || method.Span.Start < 0 || method.Span.Length < 0
                || (long)method.Span.Start + method.Span.Length > plan.SourceLength
                || plan.Throws is not { Count: > 0 and <= 256 }
                || plan.Throws.Count != throwSegments.Length
                || plan.Throws.Any(site => site is null)
                || !plan.Throws.Select(site => site.SegmentOrdinal)
                    .SequenceEqual(throwSegments.Select(segment => segment.Ordinal))) return false;
            for (int index = 0; index < plan.Throws.Count; ++index)
            {
                SemanticAsyncThrowSite site = plan.Throws[index];
                SemanticAsyncSegment segment = throwSegments[index];
                SemanticAsyncControlTransfer transfer = segment.Transfer!;
                SemanticOperation? expression = transfer.Condition;
                if (site.Span is null || expression?.Span is null
                    || site.Span != segment.Span
                    || site.Span.Start < method.Span.Start
                    || (long)site.Span.Start + site.Span.Length
                        > (long)method.Span.Start + method.Span.Length
                    || site.Span.Start < 0 || site.Span.Length < 0
                    || (long)site.Span.Start + site.Span.Length > plan.SourceLength
                    || segment.AwaitSite is not null
                    || segment.Statements is not { Count: 0 }
                    || transfer.PrimaryTarget != -1 || transfer.SecondaryTarget != -1
                    || !TryGetConstructor(site.ExceptionTypeId, out string? constructor)
                    || constructor != site.ConstructorSymbolId
                    || !document.Types.Any(type => type?.Id == site.ExceptionTypeId)
                    || expression is not
                        { Kind: "object_creation", IsSupported: true, Children.Count: 0 }
                    || expression.TypeId != site.ExceptionTypeId
                    || expression.SymbolId != site.ConstructorSymbolId
                    || expression.Span.Start < site.Span.Start
                    || expression.Span.Length < 0
                    || (long)expression.Span.Start + expression.Span.Length
                        > (long)site.Span.Start + site.Span.Length)
                    return false;
            }
        }
        return true;
    }
}

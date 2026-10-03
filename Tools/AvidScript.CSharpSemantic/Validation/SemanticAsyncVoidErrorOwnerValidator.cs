using System;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Schema 55 owns the unhandled language-error exit of an async void invocation.
// Older artifacts cannot acquire this owner by adding a field or changing only
// one half of the version pair.
public static class SemanticAsyncVoidErrorOwnerValidator
{
    public static bool IsValid(SemanticDocument? document)
    {
        if (document?.AsyncMethods is null || document.Source is null) return false;
        if (!SemanticContract.HasAsyncVoidErrorOwner(document))
            return document.SchemaVersion != SemanticContract.AsyncVoidErrorOwnerSchemaVersion
                && document.SemanticVersion != SemanticContract.AsyncVoidErrorOwnerSemanticVersion
                && document.SchemaVersion != SemanticComposableCapabilities.AsyncVoidSchemaVersion
                && document.SemanticVersion != SemanticComposableCapabilities.AsyncVoidSemanticVersion
                && document.AsyncMethods.All(method => method?.VoidErrorOwner is null);
        bool composed = SemanticComposableCapabilities.IsAsyncVoidVersion(document);
        if ((composed ? !SemanticComposableCapabilityValidator.IsValid(document)
                : document.StaticInitialization is not null || document.CapabilityManifest is not null)
            || !document.AsyncMethods.Any(method => method?.VoidErrorOwner is not null)) return false;
        foreach (SemanticAsyncMethod? method in document.AsyncMethods)
        {
            if (method?.Segments is null) return false;
            if (method.VoidErrorOwner is not { } owner)
            {
                if (method.TaskResultTypeId is null && (method.ExceptionPlan is not null
                    || method.Segments.Any(segment => segment?.SynchronousExceptionTarget is not null)))
                    return false;
                continue;
            }
            if (method.TaskResultTypeId is not null
                || method.Lowering != SemanticAsyncMethod.ContinuationCfgLowering
                || method.ExceptionPlan is not { } plan
                || plan.SourceId != document.Source.SourceId
                || plan.SourceLength != document.Source.Length
                || owner.OwnerKind != SemanticAsyncVoidErrorOwner.PrivateCarrier
                || owner.UnhandledPolicy != SemanticAsyncVoidErrorOwner.ReportToSession
                || owner.UnhandledExitSegmentOrdinal < 0
                || owner.UnhandledExitSegmentOrdinal >= method.Segments.Count
                || method.Segments.Where((segment, ordinal) => segment is null
                    || segment.Ordinal != ordinal || segment.Transfer is null
                    || segment.Statements is null).Any()) return false;
            SemanticAsyncSegment[] exits = method.Segments.Where(segment =>
                segment.Transfer?.Kind == SemanticAsyncMethod.PropagateExceptionTransferKind).ToArray();
            if (exits.Length != 1 || exits[0].Ordinal != owner.UnhandledExitSegmentOrdinal
                || exits[0].AwaitSite is not null || exits[0].Statements.Count != 0
                || exits[0].Transfer is not { Condition: null, PrimaryTarget: -1,
                    SecondaryTarget: -1, ExceptionTypeId: null, CancellationTarget: null }
                || !method.Segments.Any(segment => segment.SynchronousExceptionTarget is not null
                    || segment.Transfer?.Kind == SemanticAsyncMethod.RaiseExceptionTransferKind
                    || segment.AwaitSite?.ProducerKind is "task_call" or "task_local"
                    || segment.AwaitSite is not null && segment.Transfer?.CancellationTarget is not null)) return false;
        }
        return true;
    }
}

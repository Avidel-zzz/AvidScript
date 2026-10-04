using System.Linq;

namespace AvidScript.CSharpSemantic;

// Source capability only. Execution requires the paired Guest IR and object
// cancel-resume ABI; a source manifest does not authorize a legacy callback.
public static class SemanticObjectAwaitCancellation
{
    public const int SchemaVersion = 58;
    public const string SemanticVersion = "1.67";
    public const string CapabilityId = "async.object_await_cancellation";
    public const int CapabilityVersion = 1;
    public const int MaximumAwaitSites = 64;
    public const string ProducerKind = "object_load";
    public const string LoadedObjectTypeName = "global::AvidScript.AvidLoadedObject";

    public static bool IsVersion(SemanticDocument document) =>
        document.SchemaVersion == SchemaVersion && document.SemanticVersion == SemanticVersion;

    public static bool Has(SemanticDocument document) =>
        IsVersion(document) && SemanticComposableCapabilities.Has(document, CapabilityId);

    internal static bool HasProjectedSites(SemanticDocument document) =>
        document.AsyncMethods.Any(method => SupportsMethod(method)
            && method.Segments.Any(segment => segment.AwaitSite?.ProducerKind == ProducerKind));

    public static bool SupportsMethod(SemanticAsyncMethod method) =>
        method.ExceptionPlan?.CancellationTypeId == SemanticAsyncExceptionPlan.TaskCancellationTypeId
        && (method.TaskResultTypeId == "type:int32" || method.VoidErrorOwner is not null);

    public static bool IsStatusAwareSite(SemanticDocument document,
        SemanticAsyncMethod method, SemanticAsyncAwaitSite site) =>
        Has(document) && SupportsMethod(method) && site.ProducerKind == ProducerKind;
}

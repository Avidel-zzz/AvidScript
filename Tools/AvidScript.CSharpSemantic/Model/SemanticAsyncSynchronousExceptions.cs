using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticAsyncSynchronousExceptions
{
    // Conservative language effects, independent of the current backend's
    // optimization or call graph. Static access also includes type-init guards.
    public static bool RequiresRoute(SemanticAsyncSegment segment) =>
        segment.Statements.Any(statement => CanFail(statement.Operation))
        || segment.AwaitSite is { } site && (site.ProducerKind is "task_call" or "task_local"
            || site.Arguments.Any(CanFail)
            || site.CancellationToken is { } token && CanFail(token))
        || segment.Transfer is { Condition: { } condition, Kind:
            SemanticAsyncMethod.BranchTransferKind or SemanticAsyncMethod.ReturnTransferKind }
            && CanFail(condition);

    public static bool CanFail(SemanticOperation operation) =>
        operation.Kind is "invocation" or "property_reference" or "field_reference"
            or "object_creation" or "array_creation" or "array_element_reference"
        || operation.Children.Any(CanFail);
}

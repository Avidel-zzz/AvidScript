using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticReachability(
    [property: JsonPropertyOrder(0)] string Mode,
    [property: JsonPropertyOrder(1)] IReadOnlyList<string> RootCallableIds,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> ReachableCallableIds,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticReachableImport> ReachableImports)
{
    // Consumer-derived execution plan. Additional method targets are not public
    // entrypoints and do not change the serialized source artifact's identity.
    public static SemanticReachability ExpandForExecution(SemanticDocument document, IReadOnlyList<string> additionalTargets)
    {
        SemanticReachability expanded = SemanticReachabilityProjector.Project(document.Callables, document.ControlFlowGraphs,
            document.GameplayEventCallbacks, document.DelegateEventCallbacks, document.ContinuationCallbacks,
            document.UeTypeDeclarations, document.AsyncMethods, additionalTargets, document.ExceptionFlows);
        return expanded with { RootCallableIds = document.Reachability!.RootCallableIds, Mode = document.Reachability.Mode };
    }
}

public sealed record SemanticReachableImport(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] string Module,
    [property: JsonPropertyOrder(2)] string Name);

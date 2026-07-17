using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticReachability(
    [property: JsonPropertyOrder(0)] string Mode,
    [property: JsonPropertyOrder(1)] IReadOnlyList<string> RootCallableIds,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> ReachableCallableIds,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticReachableImport> ReachableImports);

public sealed record SemanticReachableImport(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] string Module,
    [property: JsonPropertyOrder(2)] string Name);

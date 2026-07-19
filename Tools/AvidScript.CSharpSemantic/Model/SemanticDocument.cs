using System;
using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticDocument(
    [property: JsonPropertyOrder(0)] int SchemaVersion,
    [property: JsonPropertyOrder(1)] string Language,
    [property: JsonPropertyOrder(2)] string SemanticVersion,
    [property: JsonPropertyOrder(3)] SemanticSource Source,
    [property: JsonPropertyOrder(4)] bool Succeeded,
    [property: JsonPropertyOrder(5)] IReadOnlyList<SemanticType> Types,
    [property: JsonPropertyOrder(6)] IReadOnlyList<SemanticTypeShape> TypeShapes,
    [property: JsonPropertyOrder(7)] IReadOnlyList<SemanticSymbol> Symbols,
    [property: JsonPropertyOrder(8)] IReadOnlyList<SemanticCallable> Callables,
    [property: JsonPropertyOrder(9)] IReadOnlyList<SemanticMethodBody> Methods,
    [property: JsonPropertyOrder(10)] IReadOnlyList<SemanticControlFlowGraph> ControlFlowGraphs,
    [property: JsonPropertyOrder(11)] SemanticReachability? Reachability,
    [property: JsonPropertyOrder(13)] IReadOnlyList<SemanticDiagnostic> Diagnostics)
{
    [JsonPropertyOrder(12)]
    public IReadOnlyList<SemanticStateContract> StateContracts { get; init; } =
        Array.Empty<SemanticStateContract>();
}

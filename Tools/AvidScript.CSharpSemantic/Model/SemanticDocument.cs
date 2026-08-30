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
    [property: JsonPropertyOrder(18)] IReadOnlyList<SemanticDiagnostic> Diagnostics)
{
    [JsonPropertyOrder(12)]
    public IReadOnlyList<SemanticStateContract> StateContracts { get; init; } =
        Array.Empty<SemanticStateContract>();

    [JsonPropertyOrder(13)]
    public IReadOnlyList<SemanticGameplayEventCallback> GameplayEventCallbacks { get; init; } =
        Array.Empty<SemanticGameplayEventCallback>();

    [JsonPropertyOrder(14)]
    public IReadOnlyList<SemanticDelegateEventCallback> DelegateEventCallbacks { get; init; } =
        Array.Empty<SemanticDelegateEventCallback>();

    [JsonPropertyOrder(15)]
    public IReadOnlyList<SemanticContinuationCallback> ContinuationCallbacks { get; init; } =
        Array.Empty<SemanticContinuationCallback>();

    [JsonPropertyOrder(16)]
    public IReadOnlyList<SemanticAsyncMethod> AsyncMethods { get; init; } =
        Array.Empty<SemanticAsyncMethod>();

    [JsonPropertyOrder(17)]
    public IReadOnlyList<SemanticUeTypeDeclaration> UeTypeDeclarations { get; init; } =
        Array.Empty<SemanticUeTypeDeclaration>();
}

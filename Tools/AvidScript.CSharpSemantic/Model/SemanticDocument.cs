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
    [property: JsonPropertyOrder(6)] IReadOnlyList<SemanticSymbol> Symbols,
    [property: JsonPropertyOrder(7)] IReadOnlyList<SemanticMethodBody> Methods,
    [property: JsonPropertyOrder(8)] IReadOnlyList<SemanticControlFlowGraph> ControlFlowGraphs,
    [property: JsonPropertyOrder(9)] IReadOnlyList<SemanticDiagnostic> Diagnostics);

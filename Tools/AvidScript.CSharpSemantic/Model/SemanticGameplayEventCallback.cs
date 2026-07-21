using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticGameplayEventCallback(
    [property: JsonPropertyOrder(0)] int EventType,
    [property: JsonPropertyOrder(1)] string Name,
    [property: JsonPropertyOrder(2)] string MethodSymbolId,
    [property: JsonPropertyOrder(3)] SemanticSpan Span);

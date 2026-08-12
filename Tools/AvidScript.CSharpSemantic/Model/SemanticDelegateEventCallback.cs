using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticDelegateEventCallback(
    [property: JsonPropertyOrder(0)] string SubscriptionId,
    [property: JsonPropertyOrder(1)] string ExportName,
    [property: JsonPropertyOrder(2)] string Name,
    [property: JsonPropertyOrder(3)] string MethodSymbolId,
    [property: JsonPropertyOrder(4)] SemanticSpan Span);

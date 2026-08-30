using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticSymbol(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string Kind,
    [property: JsonPropertyOrder(2)] string Name,
    [property: JsonPropertyOrder(3)] string? ContainingSymbolId,
    [property: JsonPropertyOrder(4)] string? TypeId,
    [property: JsonPropertyOrder(5)] string Signature,
    [property: JsonPropertyOrder(6)] bool IsStatic,
    [property: JsonPropertyOrder(9)] string Accessibility,
    [property: JsonPropertyOrder(10)] SemanticSpan Span)
{
    [JsonPropertyOrder(7)]
    public bool IsConst { get; init; }

    [JsonPropertyOrder(8)]
    public bool IsReadonly { get; init; }

    [JsonPropertyOrder(11)]
    public bool IsExecutableReferenceSource { get; init; }
}

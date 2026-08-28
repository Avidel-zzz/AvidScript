using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticContinuationCallback(
    [property: JsonPropertyOrder(0)] int CallbackId,
    [property: JsonPropertyOrder(1)] string Name,
    [property: JsonPropertyOrder(2)] string MethodSymbolId,
    [property: JsonPropertyOrder(4)] SemanticSpan Span)
{
    public const string NonePayloadKind = "none";
    public const string ObjectPayloadKind = "object";

    [JsonPropertyOrder(3)]
    public string PayloadKind { get; init; } = NonePayloadKind;
}

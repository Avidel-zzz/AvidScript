using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticType(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string CanonicalName,
    [property: JsonPropertyOrder(2)] string DisplayName,
    [property: JsonPropertyOrder(3)] string Kind,
    [property: JsonPropertyOrder(4)] bool IsValueType,
    [property: JsonPropertyOrder(5)] bool IsNullable);

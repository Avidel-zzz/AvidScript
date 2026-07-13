using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticConversion(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] bool Exists,
    [property: JsonPropertyOrder(2)] bool IsIdentity,
    [property: JsonPropertyOrder(3)] bool IsImplicit,
    [property: JsonPropertyOrder(4)] bool IsNumeric,
    [property: JsonPropertyOrder(5)] bool IsReference,
    [property: JsonPropertyOrder(6)] bool IsNullable,
    [property: JsonPropertyOrder(7)] bool IsUserDefined,
    [property: JsonPropertyOrder(8)] string? MethodSymbolId);

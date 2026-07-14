using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticTypeShape(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] string? ElementTypeId,
    [property: JsonPropertyOrder(2)] string? EnumUnderlyingTypeId);

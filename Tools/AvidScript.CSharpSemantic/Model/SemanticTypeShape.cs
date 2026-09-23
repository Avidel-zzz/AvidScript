using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticTypeShape(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] string? ElementTypeId,
    [property: JsonPropertyOrder(2)] string? EnumUnderlyingTypeId,
    [property: JsonPropertyOrder(3)] string? GenericArgumentTypeId = null,
    [property: JsonPropertyOrder(4)] string? GenericDefinitionTypeId = null,
    [property: JsonPropertyOrder(5)] IReadOnlyList<string>? GenericArgumentTypeIds = null);

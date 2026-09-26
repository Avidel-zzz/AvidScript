using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticMethodBody(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] SemanticOperation Root);

public sealed record SemanticOperation(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] bool IsSupported,
    [property: JsonPropertyOrder(2)] string? OperatorKind,
    [property: JsonPropertyOrder(3)] bool IsChecked,
    [property: JsonPropertyOrder(4)] bool IsLifted,
    [property: JsonPropertyOrder(5)] bool IsPostfix,
    [property: JsonPropertyOrder(6)] bool IsTryCast,
    [property: JsonPropertyOrder(7)] string? TypeId,
    [property: JsonPropertyOrder(8)] string? SymbolId,
    [property: JsonPropertyOrder(9)] IReadOnlyList<string> TypeArgumentIds,
    [property: JsonPropertyOrder(10)] SemanticConstant? Constant,
    [property: JsonPropertyOrder(11)] SemanticConversion? Conversion,
    [property: JsonPropertyOrder(12)] SemanticConversion? InputConversion,
    [property: JsonPropertyOrder(13)] SemanticConversion? OutputConversion,
    [property: JsonPropertyOrder(14)] string? CaptureId,
    [property: JsonPropertyOrder(15)] SemanticSpan Span,
    [property: JsonPropertyOrder(16)] IReadOnlyList<SemanticOperation> Children,
    [property: JsonPropertyOrder(17)] SemanticMethodDispatch? Dispatch = null)
{
    // The field symbol identifies the declaration; storage belongs to this
    // constructed type (Cache<int> and Cache<long> must never share a slot).
    [JsonPropertyOrder(18), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? StaticFieldOwnerTypeId { get; init; }
}

public sealed record SemanticMethodDispatch(
    [property: JsonPropertyOrder(0), JsonRequired] string Kind,
    [property: JsonPropertyOrder(1), JsonRequired] string? SlotMethodSymbolId,
    [property: JsonPropertyOrder(2), JsonRequired] bool IsBase);

public sealed record SemanticConstant(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string? Value);

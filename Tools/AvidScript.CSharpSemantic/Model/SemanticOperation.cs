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
    [property: JsonPropertyOrder(16)] IReadOnlyList<SemanticOperation> Children);

public sealed record SemanticConstant(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string? Value);

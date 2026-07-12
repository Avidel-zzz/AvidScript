using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpFrontend;

public sealed record FrontendAttribute(
    [property: JsonPropertyOrder(0)] string Name,
    [property: JsonPropertyOrder(1)] FrontendSpan Span,
    [property: JsonPropertyOrder(2)] IReadOnlyList<FrontendAstNode> Arguments);

public sealed record FrontendParameter(
    [property: JsonPropertyOrder(0)] string Name,
    [property: JsonPropertyOrder(1)] string Type,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> Modifiers,
    [property: JsonPropertyOrder(3)] FrontendSpan Span,
    [property: JsonPropertyOrder(4)] FrontendAstNode? DefaultValue);

public sealed record FrontendDeclaration(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string Name,
    [property: JsonPropertyOrder(2)] string? Type,
    [property: JsonPropertyOrder(3)] IReadOnlyList<string> TypeParameters,
    [property: JsonPropertyOrder(4)] IReadOnlyList<string> BaseTypes,
    [property: JsonPropertyOrder(5)] IReadOnlyList<string> Constraints,
    [property: JsonPropertyOrder(6)] string? ExplicitInterface,
    [property: JsonPropertyOrder(7)] IReadOnlyList<FrontendAstNode> Preamble,
    [property: JsonPropertyOrder(8)] IReadOnlyList<string> Modifiers,
    [property: JsonPropertyOrder(9)] FrontendSpan Span,
    [property: JsonPropertyOrder(10)] IReadOnlyList<FrontendAttribute> Attributes,
    [property: JsonPropertyOrder(11)] IReadOnlyList<FrontendParameter> Parameters,
    [property: JsonPropertyOrder(12)] IReadOnlyList<FrontendDeclaration> Members,
    [property: JsonPropertyOrder(13)] FrontendAstNode? Initializer,
    [property: JsonPropertyOrder(14)] FrontendAstNode? Body);

public sealed record FrontendAstNode(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] bool IsSupported,
    [property: JsonPropertyOrder(2)] string? Name,
    [property: JsonPropertyOrder(3)] string? Type,
    [property: JsonPropertyOrder(4)] IReadOnlyList<string> Modifiers,
    [property: JsonPropertyOrder(5)] string? Operator,
    [property: JsonPropertyOrder(6)] string? Value,
    [property: JsonPropertyOrder(7)] string? Text,
    [property: JsonPropertyOrder(8)] FrontendSpan Span,
    [property: JsonPropertyOrder(9)] IReadOnlyList<FrontendAstNode> Children);

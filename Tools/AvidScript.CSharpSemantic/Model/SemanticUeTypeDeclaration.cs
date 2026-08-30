using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticUeTypeDeclaration(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] string SymbolId,
    [property: JsonPropertyOrder(2)] string EngineName,
    [property: JsonPropertyOrder(3)] string Kind,
    [property: JsonPropertyOrder(4)] string BaseTypeId,
    [property: JsonPropertyOrder(5)] IReadOnlyList<string> Flags,
    [property: JsonPropertyOrder(6)] IReadOnlyList<SemanticUePropertyDeclaration> Properties,
    [property: JsonPropertyOrder(7)] IReadOnlyList<SemanticUeFunctionDeclaration> Functions,
    [property: JsonPropertyOrder(8)] SemanticSpan Span);

public sealed record SemanticUePropertyDeclaration(
    [property: JsonPropertyOrder(0)] string SymbolId,
    [property: JsonPropertyOrder(1)] string Name,
    [property: JsonPropertyOrder(2)] string TypeId,
    [property: JsonPropertyOrder(3)] IReadOnlyList<string> Flags,
    [property: JsonPropertyOrder(4)] string Category,
    [property: JsonPropertyOrder(5)] string ReplicatedUsing,
    [property: JsonPropertyOrder(6)] SemanticSpan Span);

public sealed record SemanticUeFunctionDeclaration(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] string Name,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> Flags,
    [property: JsonPropertyOrder(3)] string Category,
    [property: JsonPropertyOrder(4)] SemanticSpan Span);

internal sealed record SemanticUeTypeProjection(
    IReadOnlyList<SemanticUeTypeDeclaration> Declarations,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

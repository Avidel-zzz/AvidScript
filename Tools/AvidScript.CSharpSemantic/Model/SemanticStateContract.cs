using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticStateContract(
    [property: JsonPropertyOrder(0)] string OwnerTypeId,
    [property: JsonPropertyOrder(1)] string Policy,
    [property: JsonPropertyOrder(2)] int Version,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticStateFieldContract> Fields);

public sealed record SemanticStateFieldContract(
    [property: JsonPropertyOrder(0)] string SymbolId,
    [property: JsonPropertyOrder(1)] string Disposition,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> Aliases);

internal sealed record SemanticStateContractProjection(
    IReadOnlyList<SemanticStateContract> Contracts,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

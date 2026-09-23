using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

// Diagnostic projection of Roslyn's exception flow. It is not an executable CFG.
public sealed record SemanticExceptionFlow(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] string SourceId,
    [property: JsonPropertyOrder(2)] int SourceLength,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticExceptionRegion> Regions,
    [property: JsonPropertyOrder(4)] IReadOnlyList<SemanticExceptionBranch> Branches,
    [property: JsonPropertyOrder(5)] IReadOnlyList<SemanticThrowSite> Throws,
    [property: JsonPropertyOrder(6)] IReadOnlyList<SemanticCatchHandler> Catches);

public sealed record SemanticExceptionRegion(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] int ParentOrdinal,
    [property: JsonPropertyOrder(2)] string Kind,
    [property: JsonPropertyOrder(3)] int FirstBlockOrdinal,
    [property: JsonPropertyOrder(4)] int LastBlockOrdinal,
    [property: JsonPropertyOrder(5)] string? ExceptionTypeId);

public sealed record SemanticExceptionBranch(
    [property: JsonPropertyOrder(0)] int SourceBlockOrdinal,
    [property: JsonPropertyOrder(1)] int DestinationBlockOrdinal,
    [property: JsonPropertyOrder(2)] string Kind,
    [property: JsonPropertyOrder(3)] string Semantics,
    [property: JsonPropertyOrder(4)] IReadOnlyList<int> LeavingRegionOrdinals,
    [property: JsonPropertyOrder(5)] IReadOnlyList<int> EnteringRegionOrdinals,
    [property: JsonPropertyOrder(6)] IReadOnlyList<int> FinallyRegionOrdinals);

public sealed record SemanticThrowSite(
    [property: JsonPropertyOrder(0)] string Kind,
    [property: JsonPropertyOrder(1)] string? ExceptionTypeId,
    [property: JsonPropertyOrder(2)] SemanticSpan Span);

public sealed record SemanticCatchHandler(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] string? ExceptionTypeId,
    [property: JsonPropertyOrder(2)] string? ExceptionVariableSymbolId,
    [property: JsonPropertyOrder(3)] bool HasFilter,
    [property: JsonPropertyOrder(4)] SemanticSpan Span);

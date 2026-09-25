using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

// Diagnostic projection of Roslyn's exception flow and block operations.
// It is not an executable CFG until the Guest outcome and cleanup paths consume it.
public sealed record SemanticExceptionFlow(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] string SourceId,
    [property: JsonPropertyOrder(2)] int SourceLength,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticExceptionRegion> Regions,
    [property: JsonPropertyOrder(4)] IReadOnlyList<SemanticExceptionBranch> Branches,
    [property: JsonPropertyOrder(5)] IReadOnlyList<SemanticThrowSite> Throws,
    [property: JsonPropertyOrder(6)] IReadOnlyList<SemanticCatchHandler> Catches,
    [property: JsonPropertyOrder(7)] IReadOnlyList<SemanticExceptionBlock>? Blocks = null)
{
    // Kept only on rejected async source; no published Semantic contract accepts it.
    [JsonPropertyOrder(8), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public SemanticAsyncExceptionPreview? AsyncContinuationPreview { get; init; }
}

public sealed record SemanticAsyncExceptionPreview(
    [property: JsonPropertyOrder(0)] IReadOnlyList<SemanticAsyncSegment> Segments,
    [property: JsonPropertyOrder(1)] int EntrySegmentOrdinal,
    [property: JsonPropertyOrder(2)] IReadOnlyList<SemanticAsyncCompilerLocal> CompilerLocals,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticAsyncLexicalScope> LexicalScopes);

public sealed record SemanticExceptionBlock(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] string Kind,
    [property: JsonPropertyOrder(2)] bool IsReachable,
    [property: JsonPropertyOrder(3)] string ConditionKind,
    [property: JsonPropertyOrder(4)] int EnclosingRegionOrdinal,
    [property: JsonPropertyOrder(5)] IReadOnlyList<SemanticOperation> Operations,
    [property: JsonPropertyOrder(6)] SemanticOperation? BranchValue);

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
    [property: JsonPropertyOrder(4)] SemanticSpan Span,
    [property: JsonPropertyOrder(5)] int RegionOrdinal);

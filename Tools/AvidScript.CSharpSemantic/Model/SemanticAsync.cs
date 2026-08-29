using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticAsyncMethod(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] string ExportName,
    [property: JsonPropertyOrder(2)] string Lowering,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticAsyncSegment> Segments,
    [property: JsonPropertyOrder(4)] SemanticSpan Span)
{
    public const string ReentrantZeroHeapCpsLowering = "reentrant_zero_heap_cps";
}

public sealed record SemanticAsyncSegment(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] IReadOnlyList<SemanticAsyncStatement> Statements,
    [property: JsonPropertyOrder(2)] SemanticAsyncAwaitSite? AwaitSite,
    [property: JsonPropertyOrder(3)] SemanticSpan Span);

public sealed record SemanticAsyncStatement(
    [property: JsonPropertyOrder(0)] SemanticOperation Operation,
    [property: JsonPropertyOrder(1)] string? TargetSymbolId);

public sealed record SemanticAsyncAwaitSite(
    [property: JsonPropertyOrder(0)] int CallbackId,
    [property: JsonPropertyOrder(1)] string ProducerKind,
    [property: JsonPropertyOrder(2)] string PayloadKind,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticOperation> Arguments,
    [property: JsonPropertyOrder(4)] string? ResultSymbolId,
    [property: JsonPropertyOrder(5)] string? ResultTypeId,
    [property: JsonPropertyOrder(6)] SemanticSpan Span,
    [property: JsonPropertyOrder(7)] SemanticOperation? CancellationToken = null);

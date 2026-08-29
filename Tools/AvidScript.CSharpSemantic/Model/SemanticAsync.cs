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
    public const string EarlyReturnGuardOperationKind = "async_early_return_guard";
    public const string BlockOperationKind = "async_block";
    public const string LocalDeclarationOperationKind = "async_local_declaration";
    public const string IfOperationKind = "async_if";
    public const string WhileOperationKind = "async_while";
    public const string DoWhileOperationKind = "async_do_while";
    public const string ForOperationKind = "async_for";
    public const string BreakOperationKind = "async_break";
    public const string ContinueOperationKind = "async_continue";
    public const string ReturnOperationKind = "async_return";
    public const int MaximumStructuredFlowNodes = 256;
    public const int MaximumStructuredFlowDepth = 8;
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
    [property: JsonPropertyOrder(7)] SemanticOperation? CancellationToken = null,
    [property: JsonPropertyOrder(8)] int BindingOrdinal = -1,
    [property: JsonPropertyOrder(9)] string? PayloadDescriptorTypeId = null,
    [property: JsonPropertyOrder(10)] string? PayloadValueTypeId = null,
    [property: JsonPropertyOrder(11)] SemanticAsyncStateFrame? StateFrame = null);

public sealed record SemanticAsyncStateFrame(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] IReadOnlyList<SemanticAsyncStateSlot> Slots);

public sealed record SemanticAsyncStateSlot(
    [property: JsonPropertyOrder(0)] string SymbolId,
    [property: JsonPropertyOrder(1)] string TypeId);

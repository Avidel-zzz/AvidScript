using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticControlFlowGraph(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] int EntryBlockOrdinal,
    [property: JsonPropertyOrder(2)] int ExitBlockOrdinal,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticBasicBlock> Blocks);

public sealed record SemanticBasicBlock(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] string Kind,
    [property: JsonPropertyOrder(2)] bool IsReachable,
    [property: JsonPropertyOrder(3)] string ConditionKind,
    [property: JsonPropertyOrder(4)] IReadOnlyList<SemanticOperation> Operations,
    [property: JsonPropertyOrder(5)] SemanticOperation? BranchValue,
    [property: JsonPropertyOrder(6)] IReadOnlyList<SemanticControlFlowEdge> Predecessors,
    [property: JsonPropertyOrder(7)] IReadOnlyList<SemanticControlFlowEdge> Successors);

public sealed record SemanticControlFlowEdge(
    [property: JsonPropertyOrder(0)] int SourceBlockOrdinal,
    [property: JsonPropertyOrder(1)] int DestinationBlockOrdinal,
    [property: JsonPropertyOrder(2)] string Kind,
    [property: JsonPropertyOrder(3)] string Semantics);

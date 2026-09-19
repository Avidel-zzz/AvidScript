using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

// One environment instance per entry into the declared allocation scope. Cells
// retain source variable identity; multiple delegate targets share those cells.
public sealed record SemanticClosureEnvironment(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string OwnerMethodSymbolId,
    [property: JsonPropertyOrder(2)] string ScopeKind,
    [property: JsonPropertyOrder(3)] int ScopeOrdinal,
    [property: JsonPropertyOrder(4)] SemanticSpan Span,
    [property: JsonPropertyOrder(5)] IReadOnlyList<SemanticClosureCell> Cells)
{
    [JsonPropertyOrder(6)]
    public SemanticClosureAllocation? Allocation { get; init; }

    public static string GetId(string owner, string kind, int ordinal) => $"closure:{owner}:{kind}:{ordinal}";
}

// Inclusive Roslyn lifetime region. Allocate only on incoming edges, not on loop
// backedges that remain within this region. A null source denotes function entry.
public sealed record SemanticClosureAllocation(
    [property: JsonPropertyOrder(0)] int FirstBlockOrdinal,
    [property: JsonPropertyOrder(1)] int LastBlockOrdinal,
    [property: JsonPropertyOrder(2)] IReadOnlyList<SemanticClosureEntry> Entries);

public sealed record SemanticClosureEntry(
    [property: JsonPropertyOrder(0)] int? SourceBlockOrdinal,
    [property: JsonPropertyOrder(1)] int DestinationBlockOrdinal);

public sealed record SemanticClosureCell(
    [property: JsonPropertyOrder(0)] string SymbolId,
    [property: JsonPropertyOrder(1)] string TypeId,
    [property: JsonPropertyOrder(2)] string Kind);

public sealed record SemanticClosureBinding(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] bool IsDelegateTarget,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> CellSymbolIds);

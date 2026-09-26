using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public static class SemanticStaticInitialization
{
    public const int SchemaVersion = 48;
    public const string SemanticVersion = "1.57";

    public static string InitializerId(string fieldId) => "symbol:static_initializer:" + fieldId;
}

// The outer version owns initialization; the base retains the existing async,
// exception and ownership profile. Neither profile may silently erase the other.
public sealed record SemanticStaticInitializationPlan(
    [property: JsonPropertyOrder(0), JsonRequired] int BaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string BaseSemanticVersion,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<SemanticStaticTypeInitialization> Types);

public sealed record SemanticStaticTypeInitialization(
    [property: JsonPropertyOrder(0), JsonRequired] string TypeId,
    [property: JsonPropertyOrder(1), JsonRequired] bool BeforeFieldInit,
    [property: JsonPropertyOrder(2), JsonRequired] string? ConstructorMethodId,
    [property: JsonPropertyOrder(3), JsonRequired] IReadOnlyList<SemanticStaticFieldInitialization> Fields);

public sealed record SemanticStaticFieldInitialization(
    [property: JsonPropertyOrder(0), JsonRequired] string FieldSymbolId,
    [property: JsonPropertyOrder(1), JsonRequired] string SourceId,
    [property: JsonPropertyOrder(2), JsonRequired] string SourceSha256,
    [property: JsonPropertyOrder(3), JsonRequired] int SourceLength,
    [property: JsonPropertyOrder(4), JsonRequired] SemanticSpan Span,
    [property: JsonPropertyOrder(5), JsonRequired] SemanticOperation? Initializer,
    [property: JsonPropertyOrder(6), JsonRequired] SemanticControlFlowGraph? ControlFlowGraph);

using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

public sealed record SemanticCallable(
    [property: JsonPropertyOrder(0)] string MethodSymbolId,
    [property: JsonPropertyOrder(1)] string ContainingTypeId,
    [property: JsonPropertyOrder(2)] string ReturnTypeId,
    [property: JsonPropertyOrder(3)] IReadOnlyList<SemanticCallableParameter> Parameters,
    [property: JsonPropertyOrder(4)] bool IsStatic,
    [property: JsonPropertyOrder(5)] bool IsConstructor,
    [property: JsonPropertyOrder(6)] bool HasBody,
    [property: JsonPropertyOrder(7)] string? AssociatedSymbolId,
    [property: JsonPropertyOrder(8)] SemanticCallableImport? Import,
    [property: JsonPropertyOrder(9)] SemanticCallableExport? Export,
    [property: JsonPropertyOrder(10)] SemanticCallableOptimization? Optimization = null,
    [property: JsonPropertyOrder(11)] SemanticCallableDispatch? Dispatch = null);

public sealed record SemanticCallableDispatch(
    [property: JsonPropertyOrder(0), JsonRequired] bool IsVirtual,
    [property: JsonPropertyOrder(1), JsonRequired] bool IsAbstract,
    [property: JsonPropertyOrder(2), JsonRequired] bool IsOverride,
    [property: JsonPropertyOrder(3), JsonRequired] bool IsSealed,
    [property: JsonPropertyOrder(4), JsonRequired] string? OverriddenMethodSymbolId,
    [property: JsonPropertyOrder(5), JsonRequired] string? SlotMethodSymbolId,
    [property: JsonPropertyOrder(6), JsonRequired] IReadOnlyList<string> ExplicitInterfaceMethodIds);

public sealed record SemanticCallableParameter(
    [property: JsonPropertyOrder(0)] int Ordinal,
    [property: JsonPropertyOrder(1)] string SymbolId,
    [property: JsonPropertyOrder(2)] string Name,
    [property: JsonPropertyOrder(3)] string TypeId,
    [property: JsonPropertyOrder(4)] string RefKind);

public sealed record SemanticCallableImport(
    [property: JsonPropertyOrder(0)] string Module,
    [property: JsonPropertyOrder(1)] string Name);

public sealed record SemanticCallableExport(
    [property: JsonPropertyOrder(0)] string Name);

public sealed record SemanticCallableOptimization(
    [property: JsonPropertyOrder(0)] string OptimizationClass,
    [property: JsonPropertyOrder(1)] int BindingOrdinal);

using System.Collections.Generic;
using System.Text.Json.Serialization;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

public sealed record CSharpGuestDebugMap(
    [property: JsonPropertyOrder(0)] int SchemaVersion,
    [property: JsonPropertyOrder(1)] string DebugVersion,
    [property: JsonPropertyOrder(2)] string ModuleId,
    [property: JsonPropertyOrder(3)] int ImportedFunctionCount,
    [property: JsonPropertyOrder(4)] int DefinedFunctionCount,
    [property: JsonPropertyOrder(5)] CSharpGuestDebugSource Source,
    [property: JsonPropertyOrder(6)] CSharpGuestDebugProvenance Provenance,
    [property: JsonPropertyOrder(7)] IReadOnlyList<CSharpGuestDebugFunction> Functions);

public sealed record CSharpGuestDebugSource(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string Sha256);

public sealed record CSharpGuestDebugProvenance(
    [property: JsonPropertyOrder(0)] string FrontendArtifactSha256,
    [property: JsonPropertyOrder(1)] string SemanticSha256,
    [property: JsonPropertyOrder(2)] string GuestIrSha256,
    [property: JsonPropertyOrder(3)] string? WasmSha256 = null);

public sealed record CSharpGuestDebugFunction(
    [property: JsonPropertyOrder(0)] int WasmFunctionIndex,
    [property: JsonPropertyOrder(1)] string GuestFunctionId,
    [property: JsonPropertyOrder(2)] string MethodSymbolId,
    [property: JsonPropertyOrder(3)] string DisplayName,
    [property: JsonPropertyOrder(4)] SemanticSpan Span,
    [property: JsonPropertyOrder(5)] IReadOnlyList<CSharpGuestDebugSequencePoint>? SequencePoints = null);

public sealed record CSharpGuestDebugSequencePoint(
    [property: JsonPropertyOrder(0)] int WasmFunctionOffset,
    [property: JsonPropertyOrder(1)] string GuestInstructionId,
    [property: JsonPropertyOrder(2)] string SemanticOperationId,
    [property: JsonPropertyOrder(3)] string Kind,
    [property: JsonPropertyOrder(4)] bool Hidden,
    [property: JsonPropertyOrder(5)] SemanticSpan Span);

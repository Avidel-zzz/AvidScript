using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public sealed record GuestModule(
    [property: JsonPropertyOrder(0)] int SchemaVersion,
    [property: JsonPropertyOrder(1)] string IrVersion,
    [property: JsonPropertyOrder(2)] string ModuleId,
    [property: JsonPropertyOrder(3)] string Language,
    [property: JsonPropertyOrder(4)] GuestProvenance Provenance,
    [property: JsonPropertyOrder(5)] bool Succeeded,
    [property: JsonPropertyOrder(6)] GuestMemoryLayout MemoryLayout,
    [property: JsonPropertyOrder(7)] IReadOnlyList<GuestType> Types,
    [property: JsonPropertyOrder(8)] IReadOnlyList<GuestImport> Imports,
    [property: JsonPropertyOrder(9)] IReadOnlyList<GuestGlobal> Globals,
    [property: JsonPropertyOrder(10)] IReadOnlyList<GuestDataSegment> DataSegments,
    [property: JsonPropertyOrder(11)] IReadOnlyList<GuestFunction> Functions,
    [property: JsonPropertyOrder(12)] IReadOnlyList<GuestExport> Exports,
    [property: JsonPropertyOrder(13)] IReadOnlyList<GuestDiagnostic> Diagnostics);

public sealed record GuestProvenance(
    [property: JsonPropertyOrder(0)] string SourceId,
    [property: JsonPropertyOrder(1)] string SourceSha256,
    [property: JsonPropertyOrder(2)] string FrontendSha256,
    [property: JsonPropertyOrder(3)] string SemanticSha256,
    [property: JsonPropertyOrder(4)] int SemanticSchemaVersion,
    [property: JsonPropertyOrder(5)] string SemanticVersion);

public sealed record GuestImport(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string Module,
    [property: JsonPropertyOrder(2)] string Name,
    [property: JsonPropertyOrder(3)] IReadOnlyList<string> ParameterTypeIds,
    [property: JsonPropertyOrder(4)] string ReturnTypeId);

public sealed record GuestGlobal(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string TypeId,
    [property: JsonPropertyOrder(2)] bool IsMutable,
    [property: JsonPropertyOrder(3)] GuestConstant InitialValue);

public sealed record GuestExport(
    [property: JsonPropertyOrder(0)] string Name,
    [property: JsonPropertyOrder(1)] string FunctionId);

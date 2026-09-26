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
    [property: JsonPropertyOrder(13)] IReadOnlyList<GuestDiagnostic> Diagnostics)
{
    [JsonPropertyOrder(14)]
    public IReadOnlyList<GuestFunctionReference> FunctionReferences { get; init; } =
        System.Array.Empty<GuestFunctionReference>();

    [JsonPropertyOrder(15)]
    public IReadOnlyList<GuestFramedExport> FramedExports { get; init; } =
        System.Array.Empty<GuestFramedExport>();

    // Absent on IR 14 and earlier, preserving their canonical serialized bytes.
    [JsonPropertyOrder(16)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public IReadOnlyList<GuestLanguageOutcomeType>? LanguageOutcomeTypes { get; init; }

    // IR 17 only; omitted from older artifacts to preserve canonical bytes.
    [JsonPropertyOrder(17)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public GuestLanguageErrorCatalog? LanguageErrorCatalog { get; init; }

    [JsonPropertyOrder(18)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public IReadOnlyList<GuestAsyncExceptionRoute>? AsyncExceptionRoutes { get; init; }

    [JsonPropertyOrder(19)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public IReadOnlyList<GuestDirectAwaitRoute>? DirectAwaitRoutes { get; init; }

    [JsonPropertyOrder(20)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public IReadOnlyList<GuestAsyncExceptionTransfer>? AsyncExceptionTransfers { get; init; }

    [JsonPropertyOrder(21), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public GuestTaskLocalLifetimePlan? TaskLocalLifetimes { get; init; }

    [JsonPropertyOrder(22), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public GuestStaticStoragePlan? StaticStorage { get; init; }
}

// IR 25 binds the new compiler ownership profile to the concrete release and
// zeroing instructions. Removing cleanup from those sites invalidates the IR.
public sealed record GuestTaskLocalLifetimePlan(
    [property: JsonPropertyOrder(0)] string ExceptionModel,
    [property: JsonPropertyOrder(1)] IReadOnlyList<GuestTaskLocalLifetimeFunction> Functions);

public sealed record GuestTaskLocalLifetimeFunction(
    [property: JsonPropertyOrder(0)] string FunctionId,
    [property: JsonPropertyOrder(1)] IReadOnlyList<string> OwnerLocalIds,
    [property: JsonPropertyOrder(2)] IReadOnlyList<GuestTaskLocalReleaseSite> Releases,
    [property: JsonPropertyOrder(3)] IReadOnlyList<GuestTaskLocalScopeExit> ScopeExits);

public sealed record GuestTaskLocalScopeExit(
    [property: JsonPropertyOrder(0)] string SourceBlockId,
    [property: JsonPropertyOrder(1)] string ExitBlockId,
    [property: JsonPropertyOrder(2)] string TargetBlockId);

public sealed record GuestTaskLocalReleaseSite(
    [property: JsonPropertyOrder(0)] string BlockId,
    [property: JsonPropertyOrder(1)] string OwnerLocalId,
    [property: JsonPropertyOrder(2)] string TokenRegisterId,
    [property: JsonPropertyOrder(3)] string ClearedValueRegisterId);

// IR 23 binds a protected Timer await to a status-aware resume. There is no
// language-fault successor or source Task error lease on this route.
public sealed record GuestDirectAwaitRoute(
    [property: JsonPropertyOrder(0)] string MethodFunctionId,
    [property: JsonPropertyOrder(1)] int CallbackId,
    [property: JsonPropertyOrder(2)] string ProducerKind,
    [property: JsonPropertyOrder(3)] string AwaitBlockId,
    [property: JsonPropertyOrder(4)] string NormalTargetBlockId,
    [property: JsonPropertyOrder(5)] string CancellationTargetBlockId,
    [property: JsonPropertyOrder(6)] string ScheduleImportId)
{
    // IR 24 gives a directly cancelled Timer a typed Task error owner.
    [JsonPropertyOrder(7)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public GuestDirectAwaitCancellation? Cancellation { get; init; }
}

public sealed record GuestDirectAwaitCancellation(
    [property: JsonPropertyOrder(0)] string OwnerLocalId,
    [property: JsonPropertyOrder(1)] string TypeLocalId,
    [property: JsonPropertyOrder(2)] int TypeToken,
    [property: JsonPropertyOrder(3)] int SourceToken);

public sealed record GuestAsyncExceptionTransfer(
    [property: JsonPropertyOrder(0)] string MethodFunctionId,
    [property: JsonPropertyOrder(1)] string BlockId,
    [property: JsonPropertyOrder(2)] string Kind,
    [property: JsonPropertyOrder(3)] string? TargetBlockId,
    [property: JsonPropertyOrder(4)] string OwnerLocalId,
    [property: JsonPropertyOrder(5)] string TypeLocalId)
{
    [JsonPropertyOrder(6), JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public GuestAsyncRaise? Raise { get; init; }
}

public sealed record GuestAsyncRaise(
    [property: JsonPropertyOrder(0)] int TypeToken,
    [property: JsonPropertyOrder(1)] int SourceToken);

// IR 22 binds each protected Task await to all three executable successors.
// The local identities name the retained source Task and the fault type token.
public sealed record GuestAsyncExceptionRoute(
    [property: JsonPropertyOrder(0)] string MethodFunctionId,
    [property: JsonPropertyOrder(1)] int CallbackId,
    [property: JsonPropertyOrder(2)] string AwaitBlockId,
    [property: JsonPropertyOrder(3)] string NormalTargetBlockId,
    [property: JsonPropertyOrder(4)] string FaultTargetBlockId,
    [property: JsonPropertyOrder(5)] string CancellationTargetBlockId,
    [property: JsonPropertyOrder(6)] string OwnerLocalId,
    [property: JsonPropertyOrder(7)] string TypeLocalId);

// Dedicated synchronous, same-domain adapters. Ordinary exports remain subject to
// the raw-reference escape rules. Parameter kinds are value/ref/out/in, in order.
public sealed record GuestFramedExport(
    [property: JsonPropertyOrder(0)] string Name,
    [property: JsonPropertyOrder(1)] string FunctionId,
    [property: JsonPropertyOrder(2)] IReadOnlyList<string> ParameterKinds)
{
    // Optional synchronous host route: (frame address, byte count) -> success (1).
    // The original export remains the validated body. Host-selected routes also
    // contribute every possible target to the cooperative cancellation graph.
    [JsonPropertyOrder(3)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? HostImportId { get; init; }

    [JsonPropertyOrder(4)]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public IReadOnlyList<GuestHostDispatchTarget>? HostDispatchTargets { get; init; }
}

// Selectors are opaque to generic IR; only the owning host interprets their meaning.
public sealed record GuestHostDispatchTarget(
    [property: JsonPropertyOrder(0), JsonRequired] uint Selector,
    [property: JsonPropertyOrder(1), JsonRequired] string ExportName);

// A nominal signature and its closed set of addressable Guest functions.
// Imports require a Guest adapter, so host capabilities cannot become arbitrary table entries.
public sealed record GuestFunctionReference(
    [property: JsonPropertyOrder(0)] string TypeId,
    [property: JsonPropertyOrder(1)] IReadOnlyList<string> ParameterTypeIds,
    [property: JsonPropertyOrder(2)] string ReturnTypeId,
    [property: JsonPropertyOrder(3)] IReadOnlyList<string> TargetFunctionIds);

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
    [property: JsonPropertyOrder(4)] string ReturnTypeId,
    [property: JsonPropertyOrder(5)] string DispatchClass = "semantic",
    [property: JsonPropertyOrder(6)] string OptimizationClass = "none",
    [property: JsonPropertyOrder(7)] int BindingOrdinal = -1);

public sealed record GuestGlobal(
    [property: JsonPropertyOrder(0)] string Id,
    [property: JsonPropertyOrder(1)] string TypeId,
    [property: JsonPropertyOrder(2)] bool IsMutable,
    [property: JsonPropertyOrder(3)] GuestConstant InitialValue);

public sealed record GuestExport(
    [property: JsonPropertyOrder(0)] string Name,
    [property: JsonPropertyOrder(1)] string FunctionId);

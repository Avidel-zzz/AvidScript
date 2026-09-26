using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestAsyncSynchronousExceptions
{
    public const int SchemaVersion = 29;
    public const string IrVersion = "1.28";
    public const int SemanticSchemaVersion = 50;
    public const string SemanticVersion = "1.59";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    // Also recognized on the private execution-profile view. The outer reader
    // independently rejects this metadata on every other artifact version.
    internal static bool HasSourceContract(GuestModule module) =>
        module.AsyncSynchronousExceptions is not null
        && module.Provenance.SemanticSchemaVersion == SemanticSchemaVersion
        && module.Provenance.SemanticVersion == SemanticVersion;
}

public sealed record GuestAsyncSynchronousExceptionPlan(
    [property: JsonPropertyOrder(0), JsonRequired] IReadOnlyList<GuestAsyncSynchronousExceptionSite> Sites);

public sealed record GuestAsyncSynchronousExceptionSite(
    [property: JsonPropertyOrder(0), JsonRequired] string MethodFunctionId,
    [property: JsonPropertyOrder(1), JsonRequired] string FunctionId,
    [property: JsonPropertyOrder(2), JsonRequired] string CallBlockId,
    [property: JsonPropertyOrder(3), JsonRequired] int CallInstructionIndex,
    [property: JsonPropertyOrder(4), JsonRequired] string TargetBlockId,
    [property: JsonPropertyOrder(5), JsonRequired] string OwnerLocalId,
    [property: JsonPropertyOrder(6), JsonRequired] string TypeLocalId);

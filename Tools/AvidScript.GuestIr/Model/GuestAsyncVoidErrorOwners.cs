using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestAsyncVoidErrorOwners
{
    public const int SchemaVersion = 36;
    public const string IrVersion = "1.35";
    public const int SemanticSchemaVersion = 55;
    public const string SemanticVersion = "1.64";
    public const int CompositionSchemaVersion = 37;
    public const string CompositionIrVersion = "1.36";
    public const int CompositionSemanticSchemaVersion = 56;
    public const string CompositionSemanticVersion = "1.65";
    public const string CapabilityId = "error.async_void_owner";
    public const string ReportImportId = "import:language_error_report_v1";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion
        || IsCompositionVersion(module)
        || GuestObjectAwaitCancellation.IsVersion(module)
            && module.ObjectAwaitCancellation is { SourceBaseSchemaVersion: SemanticSchemaVersion,
                SourceBaseSemanticVersion: SemanticVersion } && module.AsyncVoidErrorOwners is not null;

    public static bool IsCompositionVersion(GuestModule module) =>
        module.SchemaVersion == CompositionSchemaVersion && module.IrVersion == CompositionIrVersion;

    public static bool HasSourceContract(GuestModule module) => IsVersion(module)
        && module.Language == "csharp" && (GuestObjectAwaitCancellation.IsVersion(module)
            ? GuestObjectAwaitCancellation.HasSourceContract(module)
            : IsCompositionVersion(module)
            ? module.Provenance is { SemanticSchemaVersion: CompositionSemanticSchemaVersion, SemanticVersion: CompositionSemanticVersion }
            : module.Provenance is { SemanticSchemaVersion: SemanticSchemaVersion, SemanticVersion: SemanticVersion });
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestAsyncVoidCompositionPlan(
    [property: JsonPropertyOrder(0), JsonRequired] int ContractVersion,
    [property: JsonPropertyOrder(1), JsonRequired] int SourceBaseSchemaVersion,
    [property: JsonPropertyOrder(2), JsonRequired] string SourceBaseSemanticVersion);

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestAsyncVoidErrorOwnerPlan(
    [property: JsonPropertyOrder(0), JsonRequired] IReadOnlyList<GuestAsyncVoidErrorOwner> Owners);

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestAsyncVoidErrorOwner(
    [property: JsonPropertyOrder(0), JsonRequired] string MethodFunctionId,
    [property: JsonPropertyOrder(1), JsonRequired] string OwnerLocalId,
    [property: JsonPropertyOrder(2), JsonRequired] string TypeLocalId,
    [property: JsonPropertyOrder(3), JsonRequired] string UnhandledBlockId,
    [property: JsonPropertyOrder(4), JsonRequired] IReadOnlyList<GuestAsyncVoidErrorReport> Reports);

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestAsyncVoidErrorReport(
    [property: JsonPropertyOrder(0), JsonRequired] string FunctionId,
    [property: JsonPropertyOrder(1), JsonRequired] string BlockId);

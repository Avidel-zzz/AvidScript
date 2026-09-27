using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

// IR 32 adds cancellation identity to an otherwise unchanged execution profile.
public static class GuestTaskCancellationIdentity
{
    public const int SchemaVersion = 32;
    public const string IrVersion = "1.31";
    public const string CancelImportId = "import:task_cancel_language_error_v2";
    public const string CancelImportName = "avid_task_cancel_language_error_v2";
    public const string ReadImportId = "import:task_cancellation_token_v1";
    public const string ReadImportName = "avid_task_cancellation_token_v1";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    public static bool IsBase(int schema, string version) => GuestDirectAwaitReadiness.IsBase(schema, version);

    public static GuestModule BaseProfile(GuestModule module) =>
        IsVersion(module) && module.CancellationIdentity is { } plan && IsBase(plan.BaseSchemaVersion, plan.BaseIrVersion)
            ? module with { SchemaVersion = plan.BaseSchemaVersion, IrVersion = plan.BaseIrVersion }
            : module;

    // Execution projections retain the capability plan; the independent outer
    // validator rejects this plan (and both new imports) in every older artifact.
    internal static string CancellationImportId(GuestModule module) => module.CancellationIdentity is null
        ? GuestTaskCancellationErrorValidator.ImportId : CancelImportId;
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestTaskCancellationIdentityPlan(
    [property: JsonPropertyOrder(0), JsonRequired] int BaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string BaseIrVersion);

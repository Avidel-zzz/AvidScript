using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

// An outer contract for synchronous pre-cancellation. The complete base execution
// contract is still validated; only the new readiness edges belong to IR 31.
public static class GuestDirectAwaitReadiness
{
    public const int SchemaVersion = 31;
    public const string IrVersion = "1.30";
    public const string ImportId = "import:$async:cancel_status_v1";
    public const string ImportName = "avid_continuation_cancel_status_v1";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    internal static bool IsBase(int schema, string version) => (schema, version) is
        (24, "1.23") or (25, "1.24") or (26, "1.25") or (29, "1.28") or (30, "1.29");

    public static GuestModule BaseProfile(GuestModule module) =>
        IsVersion(module) && module.DirectAwaitReadiness is { } plan && IsBase(plan.BaseSchemaVersion, plan.BaseIrVersion)
            ? module with { SchemaVersion = plan.BaseSchemaVersion, IrVersion = plan.BaseIrVersion }
            : module;
}

public sealed record GuestDirectAwaitReadinessPlan(
    [property: JsonPropertyOrder(0), JsonRequired] int BaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string BaseIrVersion,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<GuestDirectAwaitReadinessGuard> Guards);

public sealed record GuestDirectAwaitReadinessGuard(
    [property: JsonPropertyOrder(0), JsonRequired] string FunctionId,
    [property: JsonPropertyOrder(1), JsonRequired] int CallbackId,
    [property: JsonPropertyOrder(2), JsonRequired] string CheckBlockId,
    [property: JsonPropertyOrder(3), JsonRequired] string ScheduleBlockId,
    [property: JsonPropertyOrder(4), JsonRequired] string CancellationBlockId);

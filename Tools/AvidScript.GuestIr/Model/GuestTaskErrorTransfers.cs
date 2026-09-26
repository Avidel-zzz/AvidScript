using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestTaskErrorTransfers
{
    public const int SchemaVersion = 28;
    public const string IrVersion = "1.27";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;
}

public sealed record GuestTaskErrorTransferPlan(
    [property: JsonPropertyOrder(0), JsonRequired] int BaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string BaseIrVersion,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<GuestTaskErrorTransferSite> Sites);

public sealed record GuestTaskErrorTransferSite(
    [property: JsonPropertyOrder(0), JsonRequired] string FunctionId,
    [property: JsonPropertyOrder(1), JsonRequired] string CallBlockId,
    [property: JsonPropertyOrder(2), JsonRequired] int CallInstructionIndex,
    [property: JsonPropertyOrder(3), JsonRequired] string ErrorBlockId,
    [property: JsonPropertyOrder(4), JsonRequired] int FaultInstructionIndex);

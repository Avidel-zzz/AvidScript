using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestStaticStorage
{
    public const int SchemaVersion = 27;
    public const string IrVersion = "1.26";
    public const string GetOp = "managed_static_get";
    public const string SetOp = "managed_static_set";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    // Reuse the complete existing execution contract, including its provenance
    // and ownership checks. The serialized module always retains its outer version.
    public static GuestModule ExecutionProfile(GuestModule module) =>
        IsVersion(module) && module.StaticStorage is { BaseSchemaVersion: >= 4 and <= 26 } storage
            ? module with { SchemaVersion = storage.BaseSchemaVersion, IrVersion = storage.BaseIrVersion }
            : module;
}

public sealed record GuestStaticStoragePlan(
    [property: JsonPropertyOrder(0), JsonRequired] int BaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string BaseIrVersion,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<GuestStaticSlot> Slots);

public sealed record GuestStaticSlot(
    [property: JsonPropertyOrder(0), JsonRequired] string Id,
    [property: JsonPropertyOrder(1), JsonRequired] string TypeId);

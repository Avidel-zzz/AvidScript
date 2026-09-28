using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

// IR 35 reserves a versioned feature list. Admission remains closed until
// the Semantic, emitter, and native readers implement this exact contract.
public static class GuestComposableCapabilities
{
    public const int SchemaVersion = 35;
    public const string IrVersion = "1.34";

    public const string StaticStorage = "managed.static_storage";
    public const string AwaitReadiness = "async.await_readiness";
    public const string CancellationIdentity = "async.cancellation_identity";
    public const string ExceptionValues = "error.exception_values";
    public const string CancellationTokenValue = "error.cancellation_token_value";

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    public static bool IsExecutionBase(int schema, string version) =>
        (schema, version) is (14, "1.13") or (17, "1.16") or (29, "1.28");
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestCapabilityManifest(
    [property: JsonPropertyOrder(0), JsonRequired] int ExecutionBaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string ExecutionBaseIrVersion,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<GuestCapability> Capabilities)
{
    public static GuestCapabilityManifest Create(
        int executionBaseSchemaVersion,
        string executionBaseIrVersion,
        IEnumerable<GuestCapability> capabilities)
    {
        ArgumentNullException.ThrowIfNull(capabilities);
        return new(executionBaseSchemaVersion, executionBaseIrVersion,
            capabilities.OrderBy(item => item.Id, StringComparer.Ordinal).ToArray());
    }
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestCapability(
    [property: JsonPropertyOrder(0), JsonRequired] string Id,
    [property: JsonPropertyOrder(1), JsonRequired] int Version);

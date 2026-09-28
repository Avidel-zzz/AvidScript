using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

// IR 35 carries a versioned feature list. The validator currently admits the
// synchronous static-storage plus token-value pair; async combinations remain closed.
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

    public static bool Has(GuestModule module, string capabilityId) =>
        IsVersion(module) && module.CapabilityManifest?.Capabilities?.Any(capability =>
            capability is { Version: 1 } && capability.Id == capabilityId) == true;

    public static bool HasExecutionBase(GuestModule module, int schema, string version) =>
        IsVersion(module) && module.CapabilityManifest is { } manifest
        && manifest.ExecutionBaseSchemaVersion == schema
        && manifest.ExecutionBaseIrVersion == version;

    // This recognizes a declaration and its plan bases. Independent validators
    // must still check every plan, instruction, import and ownership edge.
    public static bool HasDeclaredAsyncBase29(GuestModule module) =>
        HasExecutionBase(module, 29, "1.28")
        && module.Language == "csharp"
        && module.Provenance is { SemanticSchemaVersion: 54, SemanticVersion: "1.63" }
        && Has(module, StaticStorage) && Has(module, AwaitReadiness)
        && Has(module, CancellationIdentity) && Has(module, ExceptionValues)
        && Has(module, CancellationTokenValue)
        && module.StaticStorage is { BaseSchemaVersion: 29, BaseIrVersion: "1.28" }
        && module.DirectAwaitReadiness is { BaseSchemaVersion: 29, BaseIrVersion: "1.28" }
        && module.CancellationIdentity is { BaseSchemaVersion: 29, BaseIrVersion: "1.28" }
        && module.CancellationTokens is { BaseSchemaVersion: 29, BaseIrVersion: "1.28" }
        && module.ExceptionValues is not null;

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

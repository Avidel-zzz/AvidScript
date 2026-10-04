using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace AvidScript.GuestIr;

public static class GuestObjectAwaitCancellation
{
    public const int SchemaVersion = 39;
    public const string IrVersion = "1.38";
    public const int SemanticSchemaVersion = 58;
    public const string SemanticVersion = "1.67";
    public const int ExecutionBaseSchemaVersion = 29;
    public const string ExecutionBaseIrVersion = "1.28";
    public const string CapabilityId = "async.object_await_cancellation";
    public const string ProducerKind = "object_load";
    public const string ImportModule = "avidscript";
    public const string ImportName = "avid_continuation_load_object_cancel_resume_v1";
    public const string StatusTypeId = "type:global::AvidScript.AvidContinuationStatus";
    public const string LoadedObjectTypeId = "type:global::AvidScript.AvidLoadedObject";
    public const int MaximumAwaitSites = 64;

    public static bool IsVersion(GuestModule module) =>
        module.SchemaVersion == SchemaVersion && module.IrVersion == IrVersion;

    public static bool HasSourceContract(GuestModule module) => IsVersion(module)
        && module.Language == "csharp"
        && module.Provenance is { SemanticSchemaVersion: SemanticSchemaVersion, SemanticVersion: SemanticVersion };

    public static bool IsSourceBase(int schema, string version) =>
        (schema, version) is (50, "1.59") or (52, "1.61") or (53, "1.62") or (55, "1.64");

    public static bool HasDeclaredExecutionBase(GuestModule module) =>
        HasSourceContract(module)
        && module.CapabilityManifest is { ExecutionBaseSchemaVersion: ExecutionBaseSchemaVersion,
            ExecutionBaseIrVersion: ExecutionBaseIrVersion } manifest
        && manifest.Capabilities.Any(capability => capability is { Id: CapabilityId, Version: 1 })
        && module.ObjectAwaitCancellation is { AwaitCallbackIds.Count: > 0 and <= MaximumAwaitSites } plan
        && IsSourceBase(plan.SourceBaseSchemaVersion, plan.SourceBaseSemanticVersion)
        && module.CancellationIdentity is { BaseSchemaVersion: ExecutionBaseSchemaVersion,
            BaseIrVersion: ExecutionBaseIrVersion }
        && (module.StaticStorage is null || module.StaticStorage is { BaseSchemaVersion: ExecutionBaseSchemaVersion,
            BaseIrVersion: ExecutionBaseIrVersion })
        && (module.DirectAwaitReadiness is null || module.DirectAwaitReadiness is { BaseSchemaVersion: ExecutionBaseSchemaVersion,
            BaseIrVersion: ExecutionBaseIrVersion })
        && (module.CancellationTokens is null || module.CancellationTokens is { BaseSchemaVersion: ExecutionBaseSchemaVersion,
            BaseIrVersion: ExecutionBaseIrVersion });
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record GuestObjectAwaitCancellationPlan(
    [property: JsonPropertyOrder(0), JsonRequired] int SourceBaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string SourceBaseSemanticVersion,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<int> AwaitCallbackIds);

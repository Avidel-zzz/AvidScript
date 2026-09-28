using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Serialization;

namespace AvidScript.CSharpSemantic;

// Semantic 54 is a source-backed composition envelope. Guest execution remains
// unavailable until the paired IR, emitter, and native readers admit it.
public static class SemanticComposableCapabilities
{
    public const int SchemaVersion = 54;
    public const string SemanticVersion = "1.63";
    public const int BaseSchemaVersion = SemanticContract.AsyncSynchronousExceptionSchemaVersion;
    public const string BaseSemanticVersion = SemanticContract.AsyncSynchronousExceptionSemanticVersion;

    public const string StaticStorage = "managed.static_storage";
    public const string AwaitReadiness = "async.await_readiness";
    public const string CancellationIdentity = "async.cancellation_identity";
    public const string ExceptionValues = "error.exception_values";
    public const string CancellationTokenValue = "error.cancellation_token_value";

    public static bool IsVersion(SemanticDocument document) =>
        document.SchemaVersion == SchemaVersion && document.SemanticVersion == SemanticVersion;

    public static bool Has(SemanticDocument document, string id) =>
        IsVersion(document) && document.CapabilityManifest?.Capabilities?.Any(capability =>
            capability?.Id == id && capability.Version == 1) == true;

    public static SemanticCapabilityManifest FromProjectedSource(SemanticDocument document)
    {
        ArgumentNullException.ThrowIfNull(document);
        var capabilities = new List<SemanticCapability>();
        if (document.StaticInitialization is not null)
            capabilities.Add(new(StaticStorage, 1));
        if (document.AsyncMethods.Any(method => method.Segments.Any(segment => segment.AwaitSite is not null)))
            capabilities.Add(new(AwaitReadiness, 1));
        if (document.AsyncMethods.Any(method => method.ExceptionPlan?.CancellationTypeId is not null))
            capabilities.Add(new(CancellationIdentity, 1));
        if (document.AsyncMethods.Any(method => method.ExceptionPlan is not null))
            capabilities.Add(new(ExceptionValues, 1));
        if (document.Types.Any(type => type.Id == SemanticCancellationTokens.TypeId))
            capabilities.Add(new(CancellationTokenValue, 1));
        return new(BaseSchemaVersion, BaseSemanticVersion,
            capabilities.OrderBy(capability => capability.Id, StringComparer.Ordinal).ToArray());
    }
}

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record SemanticCapabilityManifest(
    [property: JsonPropertyOrder(0), JsonRequired] int BaseSchemaVersion,
    [property: JsonPropertyOrder(1), JsonRequired] string BaseSemanticVersion,
    [property: JsonPropertyOrder(2), JsonRequired] IReadOnlyList<SemanticCapability> Capabilities);

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record SemanticCapability(
    [property: JsonPropertyOrder(0), JsonRequired] string Id,
    [property: JsonPropertyOrder(1), JsonRequired] int Version);

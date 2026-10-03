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
    public const int AsyncVoidSchemaVersion = 56;
    public const string AsyncVoidSemanticVersion = "1.65";
    public const int StaticAsyncValueSchemaVersion = 57;
    public const string StaticAsyncValueSemanticVersion = "1.66";
    public const int SynchronousBaseSchemaVersion = SemanticContract.CurrentSchemaVersion;
    public const string SynchronousBaseSemanticVersion = SemanticContract.CurrentSemanticVersion;
    public const int AsyncBaseSchemaVersion = SemanticContract.AsyncSynchronousExceptionSchemaVersion;
    public const string AsyncBaseSemanticVersion = SemanticContract.AsyncSynchronousExceptionSemanticVersion;

    public const string StaticStorage = "managed.static_storage";
    public const string AwaitReadiness = "async.await_readiness";
    public const string CancellationIdentity = "async.cancellation_identity";
    public const string ExceptionValues = "error.exception_values";
    public const string CancellationTokenValue = "error.cancellation_token_value";
    public const string AsyncVoidOwner = "error.async_void_owner";

    public static bool IsAsyncVoidVersion(SemanticDocument document) =>
        document.SchemaVersion == AsyncVoidSchemaVersion && document.SemanticVersion == AsyncVoidSemanticVersion;

    public static bool IsStaticAsyncValueVersion(SemanticDocument document) =>
        document.SchemaVersion == StaticAsyncValueSchemaVersion && document.SemanticVersion == StaticAsyncValueSemanticVersion;

    public static bool IsVersion(SemanticDocument document) =>
        document.SchemaVersion == SchemaVersion && document.SemanticVersion == SemanticVersion
        || IsAsyncVoidVersion(document) || IsStaticAsyncValueVersion(document);

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
        if (UsesTokenValues(document.Symbols, document.Methods, document.AsyncMethods))
            capabilities.Add(new(CancellationTokenValue, 1));
        bool voidOwners = document.AsyncMethods.Any(method => method.VoidErrorOwner is not null);
        if (voidOwners) capabilities.Add(new(AsyncVoidOwner, 1));
        bool asynchronous = document.AsyncMethods.Count != 0;
        bool staticAsyncValue = document.StaticInitialization is not null && !voidOwners
            && !capabilities.Any(capability => capability.Id == CancellationTokenValue)
            && document.AsyncMethods.Any(method => method.ExceptionPlan?.Catches
                .Any(handler => handler.ExceptionVariableSymbolId is not null) == true);
        bool staticTokenWithoutBinding = document.StaticInitialization is not null && !voidOwners
            && capabilities.Any(capability => capability.Id == CancellationTokenValue)
            && document.AsyncMethods.Count != 0 && !document.AsyncMethods.Any(method => method.ExceptionPlan?.Catches
                .Any(handler => handler.ExceptionVariableSymbolId is not null) == true);
        return new(voidOwners ? SemanticContract.AsyncVoidErrorOwnerSchemaVersion
                : staticAsyncValue ? SemanticContract.AsyncCatchVariableSchemaVersion
                : staticTokenWithoutBinding ? SemanticContract.CancellationTokenSchemaVersion
                : asynchronous ? AsyncBaseSchemaVersion : SynchronousBaseSchemaVersion,
            voidOwners ? SemanticContract.AsyncVoidErrorOwnerSemanticVersion
                : staticAsyncValue ? SemanticContract.AsyncCatchVariableSemanticVersion
                : staticTokenWithoutBinding ? SemanticContract.CancellationTokenSemanticVersion
                : asynchronous ? AsyncBaseSemanticVersion : SynchronousBaseSemanticVersion,
            capabilities.OrderBy(capability => capability.Id, StringComparer.Ordinal).ToArray());
    }

    internal static bool UsesTokenValues(
        IReadOnlyList<SemanticSymbol> symbols,
        IReadOnlyList<SemanticMethodBody> methods,
        IReadOnlyList<SemanticAsyncMethod> asyncMethods)
    {
        if (symbols.Any(symbol => symbol is { Kind: "field", TypeId: SemanticCancellationTokens.TypeId }))
            return true;
        var pending = new Stack<SemanticOperation>();
        foreach (SemanticMethodBody method in methods)
            if (method?.Root is { } root) pending.Push(root);
        foreach (SemanticAsyncMethod method in asyncMethods)
        {
            if (method?.Segments is null) continue;
            foreach (SemanticAsyncSegment segment in method.Segments)
            {
                if (segment?.Statements is null) continue;
                foreach (SemanticAsyncStatement statement in segment.Statements)
                    if (statement?.Operation is { } operation) pending.Push(operation);
                if (segment.Transfer?.Condition is { } condition) pending.Push(condition);
                if (segment.AwaitSite is { } site)
                {
                    if (site.CancellationToken is { } token) pending.Push(token);
                    if (site.Arguments is not null)
                        foreach (SemanticOperation argument in site.Arguments)
                            if (argument is not null) pending.Push(argument);
                }
            }
        }
        var seen = new HashSet<SemanticOperation>(ReferenceEqualityComparer.Instance);
        while (pending.Count != 0)
        {
            SemanticOperation node = pending.Pop();
            if (!seen.Add(node)) continue;
            if (seen.Count > 250_000) return false;
            if (node.TypeId == SemanticCancellationTokens.TypeId
                || node.Kind?.StartsWith("cancellation_token_", StringComparison.Ordinal) == true)
                return true;
            if (node.Children is not null)
                foreach (SemanticOperation child in node.Children)
                    if (child is not null) pending.Push(child);
        }
        return false;
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

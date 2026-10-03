using System;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticComposableCapabilityValidator
{
    public static bool IsValid(SemanticDocument? document)
    {
        if (document is null) return false;
        if (!SemanticComposableCapabilities.IsVersion(document))
            return document.CapabilityManifest is null
                && document.SchemaVersion != SemanticComposableCapabilities.SchemaVersion
                && document.SemanticVersion != SemanticComposableCapabilities.SemanticVersion
                && document.SchemaVersion != SemanticComposableCapabilities.AsyncVoidSchemaVersion
                && document.SemanticVersion != SemanticComposableCapabilities.AsyncVoidSemanticVersion;
        bool voidOwners = SemanticComposableCapabilities.IsAsyncVoidVersion(document);
        if (document.Language != "csharp" || document.Source is null
            || document.Types is null || document.Symbols is null
            || document.Methods is null || document.AsyncMethods is null
            || document.Types.Any(type => type is null || string.IsNullOrWhiteSpace(type.Id))
            || !voidOwners && !document.Types.Any(type => type.Id == SemanticCancellationTokens.TypeId)
            || document.AsyncMethods.Any(method => method?.Segments is null
                || method.Segments.Any(segment => segment is null))
            || !voidOwners && document.StaticInitialization is not { Types: { Count: > 0 } }
            || document.CapabilityManifest is not { Capabilities: { Count: >= 2 and <= 6 } declared } manifest
            || document.StaticInitialization is { } staticPlan
                && (staticPlan.Types is not { Count: > 0 }
                    || staticPlan.BaseSchemaVersion != manifest.BaseSchemaVersion
                    || staticPlan.BaseSemanticVersion != manifest.BaseSemanticVersion)
            || declared.Any(capability => capability is null
                || string.IsNullOrWhiteSpace(capability.Id) || capability.Version != 1))
            return false;
        var expected = SemanticComposableCapabilities.FromProjectedSource(document).Capabilities;
        if (voidOwners)
            return document.AsyncMethods.Any(method => method.VoidErrorOwner is not null)
                && manifest.BaseSchemaVersion == SemanticContract.AsyncVoidErrorOwnerSchemaVersion
                && manifest.BaseSemanticVersion == SemanticContract.AsyncVoidErrorOwnerSemanticVersion
                && expected.Any(capability => capability.Id is SemanticComposableCapabilities.StaticStorage
                    or SemanticComposableCapabilities.CancellationTokenValue)
                && declared.SequenceEqual(expected)
                && declared.Select(capability => capability.Id).Distinct(StringComparer.Ordinal).Count() == declared.Count;
        bool synchronous = document.AsyncMethods.Count == 0
            && manifest.BaseSchemaVersion == SemanticComposableCapabilities.SynchronousBaseSchemaVersion
            && manifest.BaseSemanticVersion == SemanticComposableCapabilities.SynchronousBaseSemanticVersion
            && expected.Count == 2;
        bool asynchronous = document.AsyncMethods.Count != 0
            && manifest.BaseSchemaVersion == SemanticComposableCapabilities.AsyncBaseSchemaVersion
            && manifest.BaseSemanticVersion == SemanticComposableCapabilities.AsyncBaseSemanticVersion
            && expected.Count == 5;
        return (synchronous || asynchronous) && declared.SequenceEqual(expected)
            && declared.Select(capability => capability.Id).Distinct(StringComparer.Ordinal).Count() == declared.Count;
    }
}

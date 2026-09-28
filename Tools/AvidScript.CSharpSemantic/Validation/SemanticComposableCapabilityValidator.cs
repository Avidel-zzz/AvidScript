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
                && document.SemanticVersion != SemanticComposableCapabilities.SemanticVersion;
        if (document.Language != "csharp" || document.Source is null
            || document.Types is null || document.AsyncMethods is null
            || document.AsyncMethods.Any(method => method?.Segments is null
                || method.Segments.Any(segment => segment is null))
            || document.StaticInitialization is not { Types: { Count: > 0 } } staticPlan
            || staticPlan.BaseSchemaVersion != SemanticComposableCapabilities.BaseSchemaVersion
            || staticPlan.BaseSemanticVersion != SemanticComposableCapabilities.BaseSemanticVersion
            || document.CapabilityManifest is not { Capabilities: { Count: 5 } declared } manifest
            || manifest.BaseSchemaVersion != SemanticComposableCapabilities.BaseSchemaVersion
            || manifest.BaseSemanticVersion != SemanticComposableCapabilities.BaseSemanticVersion
            || declared.Any(capability => capability is null
                || string.IsNullOrWhiteSpace(capability.Id) || capability.Version != 1))
            return false;
        var expected = SemanticComposableCapabilities.FromProjectedSource(document).Capabilities;
        return expected.Count == 5 && declared.SequenceEqual(expected)
            && declared.Select(capability => capability.Id).Distinct(StringComparer.Ordinal).Count() == 5;
    }
}

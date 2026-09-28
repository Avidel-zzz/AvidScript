using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestComposableCapabilityValidator
{
    private const int MaximumCapabilities = 16;

    internal static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.InputArtifact;
        GuestCapabilityManifest? manifest = module.CapabilityManifest;
        if (!GuestComposableCapabilities.IsVersion(module))
        {
            if (manifest is not null)
                Add("A capability manifest cannot be attached to an older or unknown IR version.");
            return;
        }

        if (manifest is null)
        {
            Add("IR 35 requires an explicit capability manifest.");
            return;
        }

        if (!GuestComposableCapabilities.IsExecutionBase(
                manifest.ExecutionBaseSchemaVersion, manifest.ExecutionBaseIrVersion))
            Add("The execution base must be an exact supported schema/version pair.");

        if (module.Language == "csharp"
            && (module.Provenance.SemanticSchemaVersion != 54
                || module.Provenance.SemanticVersion != "1.63"))
            Add("C# IR 35 requires the paired Semantic 54/1.63 source contract.");

        IReadOnlyList<GuestCapability> capabilities = manifest.Capabilities;
        if (capabilities.Count > MaximumCapabilities)
        {
            Add("The capability list exceeds its fixed size limit.");
            return;
        }

        HashSet<string> declared = new(StringComparer.Ordinal);
        string previous = string.Empty;
        foreach (GuestCapability capability in capabilities)
        {
            if (string.IsNullOrWhiteSpace(capability.Id) || capability.Version != 1
                || capability.Id is not (
                    GuestComposableCapabilities.StaticStorage
                    or GuestComposableCapabilities.AwaitReadiness
                    or GuestComposableCapabilities.CancellationIdentity
                    or GuestComposableCapabilities.ExceptionValues
                    or GuestComposableCapabilities.CancellationTokenValue))
                Add("The capability list contains an unknown name or version.");
            if (string.CompareOrdinal(previous, capability.Id) >= 0 || !declared.Add(capability.Id))
                Add("Capabilities must be unique and ordered by ordinal ID.");
            previous = capability.Id;
        }

        RequirePlan(GuestComposableCapabilities.StaticStorage, module.StaticStorage is not null);
        RequirePlan(GuestComposableCapabilities.AwaitReadiness, module.DirectAwaitReadiness is not null);
        RequirePlan(GuestComposableCapabilities.CancellationIdentity, module.CancellationIdentity is not null);
        RequirePlan(GuestComposableCapabilities.ExceptionValues, module.ExceptionValues is not null);
        RequirePlan(GuestComposableCapabilities.CancellationTokenValue, module.CancellationTokens is not null);

        int baseSchema = manifest.ExecutionBaseSchemaVersion;
        string baseVersion = manifest.ExecutionBaseIrVersion;
        if (module.StaticStorage is { } storage
            && (storage.BaseSchemaVersion != baseSchema || storage.BaseIrVersion != baseVersion)
            || module.DirectAwaitReadiness is { } readiness
            && (readiness.BaseSchemaVersion != baseSchema || readiness.BaseIrVersion != baseVersion)
            || module.CancellationIdentity is { } identity
            && (identity.BaseSchemaVersion != baseSchema || identity.BaseIrVersion != baseVersion)
            || module.CancellationTokens is { } tokens
            && (tokens.BaseSchemaVersion != baseSchema || tokens.BaseIrVersion != baseVersion))
            Add("Every carried plan must name the same exact execution base.");

        if (baseSchema != 29 && (declared.Contains(GuestComposableCapabilities.AwaitReadiness)
            || declared.Contains(GuestComposableCapabilities.CancellationIdentity)
            || declared.Contains(GuestComposableCapabilities.ExceptionValues)))
            Add("Async readiness, cancellation identity, and exception values require base 29/1.28.");
        if (declared.Contains(GuestComposableCapabilities.CancellationIdentity)
            && !declared.Contains(GuestComposableCapabilities.AwaitReadiness))
            Add("Cancellation identity requires await readiness.");

        // Only this complete instruction/plan pair is admitted by the real-module
        // validators. Async composition remains closed until its ownership and
        // exception plans can be checked on the unmodified IR 35 artifact.
        if ((baseSchema, baseVersion) is not ((14, "1.13") or (17, "1.16"))
            || declared.Count != 2
            || !declared.Contains(GuestComposableCapabilities.StaticStorage)
            || !declared.Contains(GuestComposableCapabilities.CancellationTokenValue))
            Add("IR 35 currently validates only synchronous static storage with token values on base 14/1.13 or 17/1.16.");
        if (baseSchema == 14 && (module.LanguageOutcomeTypes is not null || module.LanguageErrorCatalog is not null)
            || baseSchema == 17 && (module.LanguageOutcomeTypes is not { Count: > 0 }
                || module.LanguageErrorCatalog is null))
            Add("The execution base requires its exact language-error outcome and catalog profile.");
        if (module.AsyncExceptionRoutes is not null || module.DirectAwaitRoutes is not null
            || module.AsyncExceptionTransfers is not null || module.TaskLocalLifetimes is not null
            || module.TaskErrorTransfers is not null || module.AsyncSynchronousExceptions is not null)
            Add("The synchronous IR 35 base cannot carry undeclared async metadata.");

        void RequirePlan(string id, bool present)
        {
            if (declared.Contains(id) != present)
                Add($"Capability '{id}' and its plan must be present together.");
        }

        void Add(string message) => context.Add("ASIR1042", message);
    }
}

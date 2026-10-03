using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestComposableCapabilityValidator
{
    private const int MaximumCapabilities = 16;

    internal static void Validate(GuestValidationContext context)
    {
        GuestModule module = context.InputArtifact;
        bool voidOwners = GuestAsyncVoidErrorOwners.IsVersion(module);
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

        if (module.Language is not ("csharp" or "guest-ir"))
            Add("IR 35 requires a recognized source language until its frontend contract is defined.");

        if (module.Language == "csharp"
            && (module.Provenance.SemanticSchemaVersion != GuestComposableCapabilities.ExpectedSemanticSchema(module)
                || module.Provenance.SemanticVersion != GuestComposableCapabilities.ExpectedSemanticVersion(module)))
            Add("Composed C# IR requires its exact paired Semantic source contract.");

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
                    or GuestComposableCapabilities.CancellationTokenValue)
                && !(voidOwners && capability.Id == GuestAsyncVoidErrorOwners.CapabilityId))
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
        RequirePlan(GuestAsyncVoidErrorOwners.CapabilityId, module.AsyncVoidErrorOwners is not null);
        if (GuestAsyncVoidErrorOwners.IsCompositionVersion(module)
            && !declared.Contains(GuestComposableCapabilities.StaticStorage)
            && !declared.Contains(GuestComposableCapabilities.CancellationTokenValue))
            Add("IR37 async void composition requires an actual static or token plan.");

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
        if (!voidOwners && declared.Contains(GuestComposableCapabilities.CancellationIdentity)
            && !declared.Contains(GuestComposableCapabilities.AwaitReadiness))
            Add("Cancellation identity requires await readiness.");

        bool synchronousProfile = ((baseSchema, baseVersion) is (14, "1.13") or (17, "1.16"))
            && declared.Count == 2
            && declared.Contains(GuestComposableCapabilities.StaticStorage)
            && declared.Contains(GuestComposableCapabilities.CancellationTokenValue);
        bool asynchronousProfile = GuestComposableCapabilities.HasDeclaredAsyncBase29(module)
            && (voidOwners || declared.Count == 5);
        if (voidOwners ? !asynchronousProfile : !synchronousProfile && !asynchronousProfile)
            Add("IR 35 requires the exact synchronous static/token pair or the five-capability async base 29 profile.");
        if (baseSchema == 14 && (module.LanguageOutcomeTypes is not null || module.LanguageErrorCatalog is not null)
            || (baseSchema is 17 or 29) && (module.LanguageOutcomeTypes is null
                || !voidOwners && module.LanguageOutcomeTypes.Count == 0 || module.LanguageErrorCatalog is null))
            Add("The execution base requires its exact language-error outcome and catalog profile.");
        if (baseSchema == 29)
        {
            if (module.AsyncExceptionRoutes is null || module.DirectAwaitRoutes is null
                || module.AsyncExceptionTransfers is null || module.TaskLocalLifetimes is null
                || module.AsyncSynchronousExceptions is null || module.TaskErrorTransfers is not null)
                Add("The IR 35 async base requires complete await, cancellation, owner and synchronous-error plans without legacy Task transfers.");
        }
        else if (module.AsyncExceptionRoutes is not null || module.DirectAwaitRoutes is not null
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

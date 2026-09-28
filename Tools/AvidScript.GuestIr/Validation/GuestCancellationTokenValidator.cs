using System;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestCancellationTokenValidator
{
    internal static void Validate(GuestValidationContext context)
    {
        var module = context.InputArtifact;
        var types = module.Types.Where(type => type.Id == GuestCancellationTokens.TypeId).ToArray();
        var readers = module.Imports.Where(import =>
            import.Id.StartsWith("import:exception_cancellation_token_", StringComparison.Ordinal)
            || import.Name.StartsWith("avid_exception_cancellation_token_", StringComparison.Ordinal)).ToArray();
        var fields = module.Types.SelectMany(type => type.Fields.Select(field => (Owner: type.Id, Field: field)))
            .Where(item => item.Field.Id.StartsWith("field:$cancellation_token:", StringComparison.Ordinal)).ToArray();
        bool composable = GuestComposableCapabilities.Has(module, GuestComposableCapabilities.CancellationTokenValue);
        if (!GuestCancellationTokens.IsVersion(module) && !composable)
        {
            if (module.CancellationTokens is not null || types.Length != 0 || readers.Length != 0 || fields.Length != 0
                || module.Provenance.SemanticSchemaVersion == GuestCancellationTokens.SemanticSchemaVersion
                || module.Provenance.SemanticVersion == GuestCancellationTokens.SemanticVersion)
                Add(GuestComposableCapabilities.IsVersion(module)
                    ? "IR 35 token values require their declared capability and canonical plan."
                    : "Cancellation token values require paired Semantic 53/1.62 and Guest IR 34/1.33.");
            return;
        }
        if ((!composable && module.Language != "csharp") || module.CancellationTokens is not { } plan
            || !GuestCancellationTokens.IsBase(plan.BaseSchemaVersion, plan.BaseIrVersion)
            || !composable && (module.Provenance.SemanticSchemaVersion != GuestCancellationTokens.SemanticSchemaVersion
                || module.Provenance.SemanticVersion != GuestCancellationTokens.SemanticVersion
                || module.StaticStorage is not null)
            || composable && (plan.BaseSchemaVersion, plan.BaseIrVersion) is not ((14, "1.13") or (17, "1.16"))
            || module.TaskErrorTransfers is not null)
        {
            Add("Token values require their exact source contract and a supported execution base.");
            return;
        }
        if (module.Types.Where(type => type.Id == "type:int64").ToArray() is not [
            { Kind: "scalar", Storage: "i64", Size: 8, Alignment: 8, Fields.Count: 0, ElementTypeId: null, UnderlyingTypeId: null }])
            Add("Cancellation token identity requires the canonical int64 scalar layout.");
        if (types is not [{ Kind: "struct", Storage: "memory", Size: 8, Alignment: 8,
                ElementTypeId: null, UnderlyingTypeId: null, Fields: [{ Id: GuestCancellationTokens.FieldId,
                    Name: "identity", TypeId: "type:int64", Offset: 0 }] }]
            || fields.Length != 1 || fields[0].Owner != GuestCancellationTokens.TypeId)
            Add("CancellationToken has exactly one int64 identity field in an eight-byte nominal value layout.");
        if (readers.Length > 1 || readers.Length == 1 && (!GuestCancellationTokens.HasReaderProfile(module)
            || !GuestCancellationTokens.IsReader(readers[0])
            || module.Types.Where(type => type.Id == GuestCancellationTokens.RootTypeId).ToArray() is not [
                { Kind: "managed_ref", Storage: "i64", Size: 8, Alignment: 8, Fields.Count: 0, UnderlyingTypeId: null }]))
            Add("Exception token reader requires an error-capable profile and its canonical managed-root signature.");
        if (plan.BaseSchemaVersion != 29 && (module.CancellationIdentity is not null || module.ExceptionValues is not null
            || module.AsyncSynchronousExceptions is not null || module.TaskLocalLifetimes is not null
            || module.AsyncExceptionRoutes is not null || module.AsyncExceptionTransfers is not null
            || module.DirectAwaitRoutes is not null || module.DirectAwaitReadiness is not null))
            Add("Token values cannot drop their async ownership metadata into a synchronous execution base.");
        if (plan.BaseSchemaVersion == 14 && (module.LanguageErrorCatalog is not null || module.LanguageOutcomeTypes is not null))
            Add("Plain token values do not carry an exception execution contract.");
        if (plan.BaseSchemaVersion == 29 && module.Imports.Any(import =>
            import.Id == GuestTaskCancellationErrorValidator.ImportId || import.Name == GuestTaskCancellationErrorValidator.ImportName))
            Add("Token-aware cancellation must publish identity with the v2 writer.");
        if (module.CancellationIdentity is { } identity
            && (identity.BaseSchemaVersion != plan.BaseSchemaVersion || identity.BaseIrVersion != plan.BaseIrVersion))
            Add("Cancellation identity and token value execution bases must agree.");

        void Add(string message) => context.Add("ASIR1041", message);
    }
}

using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpStaticInitializationCompiler
{
    public static bool TryLower(SemanticDocument source, string semanticSha256,
        out GuestModule? module, out string? error)
    {
        module = null;
        if (!CSharpStaticSourcePreparation.TryPrepare(source, out var ordinary, out var execution, out error)) return false;
        bool voidComposition = SemanticComposableCapabilities.IsAsyncVoidVersion(source);
        bool staticAsyncValueComposition = SemanticComposableCapabilities.IsStaticAsyncValueVersion(source);
        bool synchronousTokenComposition = !voidComposition && !staticAsyncValueComposition && SemanticComposableCapabilities.IsVersion(source)
            && source.AsyncMethods.Count == 0;
        bool asynchronousTokenComposition = !voidComposition && !staticAsyncValueComposition && SemanticComposableCapabilities.IsVersion(source)
            && source.AsyncMethods.Count != 0;
        if (ordinary!.ExceptionFlows is not null
            || !synchronousTokenComposition && SemanticContract.HasAsyncSynchronousExceptions(ordinary))
        {
            if (!CSharpLanguageErrorCompiler.TryLower(ordinary, semanticSha256, out var compilation,
                    out error, deferComposedValidation: asynchronousTokenComposition || voidComposition || staticAsyncValueComposition)) return false;
            GuestModule restored = compilation!.Module with { Provenance = compilation.Module.Provenance with
            { SemanticSchemaVersion = source.SchemaVersion, SemanticVersion = source.SemanticVersion } };
            if (voidComposition)
                restored = CSharpAsyncVoidErrorLowerer.Wrap(source, restored);
            else if (staticAsyncValueComposition)
            {
                bool tokenBase = source.CapabilityManifest!.BaseSchemaVersion == SemanticContract.CancellationTokenSchemaVersion;
                if (restored is not { StaticStorage: { BaseSchemaVersion: 29, BaseIrVersion: "1.28" },
                    CancellationIdentity: { BaseSchemaVersion: 30, BaseIrVersion: "1.29" },
                    AsyncVoidErrorOwners: null }
                    || (tokenBase
                        ? restored is not { SchemaVersion: GuestCancellationTokens.SchemaVersion, IrVersion: GuestCancellationTokens.IrVersion,
                            CancellationTokens: { BaseSchemaVersion: 29, BaseIrVersion: "1.28" } }
                        : restored is not { SchemaVersion: GuestExceptionValues.SchemaVersion, IrVersion: GuestExceptionValues.IrVersion,
                            ExceptionValues: { Bindings.Count: > 0 }, CancellationTokens: null }))
                { error = "Static async value composition requires the complete source-backed execution plan chain."; return false; }
                if (restored.DirectAwaitReadiness is not null
                    && restored.DirectAwaitReadiness is not { BaseSchemaVersion: 30, BaseIrVersion: "1.29", Guards.Count: > 0 })
                { error = "Static async value composition can carry only actual validated readiness guards."; return false; }
                var catchCapabilities = new List<GuestCapability> {
                    new(GuestComposableCapabilities.StaticStorage, 1),
                    new(GuestComposableCapabilities.CancellationIdentity, 1) };
                if (restored.ExceptionValues is not null) catchCapabilities.Add(new(GuestComposableCapabilities.ExceptionValues, 1));
                if (restored.CancellationTokens is not null) catchCapabilities.Add(new(GuestComposableCapabilities.CancellationTokenValue, 1));
                if (restored.DirectAwaitReadiness is not null) catchCapabilities.Add(new(GuestComposableCapabilities.AwaitReadiness, 1));
                restored = restored with {
                    SchemaVersion = GuestComposableCapabilities.StaticAsyncValueSchemaVersion,
                    IrVersion = GuestComposableCapabilities.StaticAsyncValueIrVersion,
                    DirectAwaitReadiness = restored.DirectAwaitReadiness is { } catchReady
                        ? catchReady with { BaseSchemaVersion = 29, BaseIrVersion = "1.28" } : null,
                    CancellationIdentity = restored.CancellationIdentity with { BaseSchemaVersion = 29, BaseIrVersion = "1.28" },
                    CapabilityManifest = GuestCapabilityManifest.Create(29, "1.28", catchCapabilities),
                    StaticAsyncValueComposition = new(source.CapabilityManifest.BaseSchemaVersion, source.CapabilityManifest.BaseSemanticVersion),
                };
            }
            else if (asynchronousTokenComposition)
            {
                // The legacy wrappers were built around IR 30. IR 35 declares
                // their shared IR 29 execution base only after the expected
                // source-derived plans exist. Final validation still owns admission.
                if (restored is not {
                    SchemaVersion: GuestCancellationTokens.SchemaVersion,
                    IrVersion: GuestCancellationTokens.IrVersion,
                    StaticStorage: { BaseSchemaVersion: 29, BaseIrVersion: "1.28" },
                    DirectAwaitReadiness: { BaseSchemaVersion: 30, BaseIrVersion: "1.29", Guards.Count: > 0 },
                    CancellationIdentity: { BaseSchemaVersion: 30, BaseIrVersion: "1.29" },
                    CancellationTokens: { BaseSchemaVersion: 29, BaseIrVersion: "1.28" },
                    ExceptionValues: not null,
                })
                {
                    error = "Async capability composition requires the complete source-derived IR 29/30 plan chain.";
                    return false;
                }
                restored = restored with
                {
                    SchemaVersion = GuestComposableCapabilities.SchemaVersion,
                    IrVersion = GuestComposableCapabilities.IrVersion,
                    DirectAwaitReadiness = restored.DirectAwaitReadiness with
                        { BaseSchemaVersion = 29, BaseIrVersion = "1.28" },
                    CancellationIdentity = restored.CancellationIdentity with
                        { BaseSchemaVersion = 29, BaseIrVersion = "1.28" },
                    CapabilityManifest = GuestCapabilityManifest.Create(29, "1.28", new[] {
                        new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
                        new GuestCapability(GuestComposableCapabilities.AwaitReadiness, 1),
                        new GuestCapability(GuestComposableCapabilities.CancellationIdentity, 1),
                        new GuestCapability(GuestComposableCapabilities.ExceptionValues, 1),
                        new GuestCapability(GuestComposableCapabilities.CancellationTokenValue, 1),
                    }),
                };
            }
            var restoredValidation = GuestModuleValidator.Validate(restored);
            if (!restoredValidation.Succeeded)
            { error = "Static language-error module failed original-provenance validation: "
                + string.Join(" | ", restoredValidation.Diagnostics.Select(item => item.Code + ": " + item.Message)); return false; }
            module = restored;
            return true;
        }
        var lowered = CSharpGuestLowerer.Lower(ordinary!, semanticSha256);
        if (!lowered.Succeeded || lowered.Module is not { } input)
        { error = "Static initializer source lowering failed: " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)); return false; }
        var sourceIds = ordinary!.Callables.Where(callable => callable.HasBody)
            .Select(callable => CSharpGuestIds.Function(callable.MethodSymbolId)).ToHashSet(StringComparer.Ordinal);
        var affected = input.Functions.Where(function => sourceIds.Contains(function.Id)).Select(function => function.Id).ToHashSet(StringComparer.Ordinal);
        var exports = input.Exports.Where(export => affected.Contains(export.FunctionId)).ToArray();
        var originals = input.Functions.ToDictionary(function => function.Id, StringComparer.Ordinal);
        GuestModule? outcomes;
        GuestModule unexported = input with { Exports = input.Exports.Except(exports).ToArray() };
        if (synchronousTokenComposition
            ? !CSharpLanguageOutcomeRewriter.TryRewriteForStaticTokenComposition(ordinary, unexported,
                affected, out outcomes, out error)
            : !CSharpLanguageOutcomeRewriter.TryRewrite(ordinary, unexported,
                affected, out outcomes, out error)) return false;
        var sourceSites = execution!.Types.Select(type => (type.SourceId, type.SourceLength, type.Span)).Distinct()
            .OrderBy(site => site.SourceId, StringComparer.Ordinal).ThenBy(site => site.Span.Start).ThenBy(site => site.Span.Length).ToArray();
        var catalog = new GuestLanguageErrorCatalog(new[] { new GuestLanguageErrorTypeToken(1, CSharpStaticInitializationGuards.ExceptionType) },
            sourceSites.Select((site, index) => new GuestLanguageErrorSourceToken(index + 1, site.SourceId, site.SourceLength,
                site.Span.Start, site.Span.Length, site.Span.Line, site.Span.Column, site.Span.EndLine, site.Span.EndColumn)).ToArray());
        var guardIds = execution.Types.Select(type => CSharpStaticInitializationGuards.FunctionId(type.TypeId)).ToHashSet(StringComparer.Ordinal);
        GuestModule candidate = outcomes! with
        {
            SchemaVersion = outcomes!.StaticStorage is null ? 17 : GuestStaticStorage.SchemaVersion,
            IrVersion = outcomes.StaticStorage is null ? "1.16" : GuestStaticStorage.IrVersion,
            StaticStorage = outcomes.StaticStorage is null ? null : outcomes.StaticStorage with { BaseSchemaVersion = 17, BaseIrVersion = "1.16" },
            LanguageErrorCatalog = catalog,
            Functions = outcomes.Functions.Where(function => !guardIds.Contains(function.Id)).ToArray(),
            Provenance = outcomes.Provenance with { SemanticSchemaVersion = source.SchemaVersion, SemanticVersion = source.SemanticVersion },
        };
        var initializers = execution.Types.Select(type => new CSharpStaticInitializer(type.TypeId, CSharpGuestIds.Function(type.BodyId),
            Array.FindIndex(sourceSites, site => site == (type.SourceId, type.SourceLength, type.Span)) + 1)).ToArray();
        GuestModule? guarded;
        if (synchronousTokenComposition
            ? !CSharpStaticInitializationGuards.TryComposeDeferred(candidate, initializers,
                out guarded, out error)
            : !CSharpStaticInitializationGuards.TryCompose(candidate, initializers,
                out guarded, out error)) return false;
        if (!CSharpLanguageErrorEntryAdapter.TryAdd(guarded!, exports, originals, out var adapted, out error)) return false;
        if (synchronousTokenComposition)
            adapted = adapted! with
            {
                SchemaVersion = GuestComposableCapabilities.SchemaVersion,
                IrVersion = GuestComposableCapabilities.IrVersion,
                CancellationTokens = new(17, "1.16"),
                CapabilityManifest = GuestCapabilityManifest.Create(17, "1.16", new[] {
                    new GuestCapability(GuestComposableCapabilities.StaticStorage, 1),
                    new GuestCapability(GuestComposableCapabilities.CancellationTokenValue, 1),
                }),
            };
        var validation = GuestModuleValidator.Validate(adapted!);
        if (!validation.Succeeded)
        { error = "Static source module failed final validation: " + string.Join(" | ", validation.Diagnostics.Select(item => item.Message)); return false; }
        module = adapted;
        return true;
    }
}

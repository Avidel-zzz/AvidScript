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
        if (ordinary!.ExceptionFlows is not null)
        {
            if (!CSharpLanguageErrorCompiler.TryLower(ordinary, semanticSha256, out var compilation, out error)) return false;
            var restored = compilation!.Module with { Provenance = compilation.Module.Provenance with
            { SemanticSchemaVersion = source.SchemaVersion, SemanticVersion = source.SemanticVersion } };
            if (!GuestModuleValidator.Validate(restored).Succeeded)
            { error = "Static language-error module failed original-provenance validation."; return false; }
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
        if (!CSharpLanguageOutcomeRewriter.TryRewrite(ordinary, input with { Exports = input.Exports.Except(exports).ToArray() },
                affected, out var outcomes, out error)) return false;
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
        if (!CSharpStaticInitializationGuards.TryCompose(candidate, initializers, out var guarded, out error)
            || !CSharpLanguageErrorEntryAdapter.TryAdd(guarded!, exports, originals, out var adapted, out error)) return false;
        var validation = GuestModuleValidator.Validate(adapted!);
        if (!validation.Succeeded)
        { error = "Static source module failed final validation: " + string.Join(" | ", validation.Diagnostics.Select(item => item.Message)); return false; }
        module = adapted;
        return true;
    }
}

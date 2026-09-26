using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

// Created only from a validated source contract, never deserialized from IR.
// Intermediate modules cannot leave the compiler until composition validates.
internal sealed class CSharpAsyncSynchronousExecutionContext
{
    private static readonly ConditionalWeakTable<SemanticDocument, CSharpAsyncSynchronousExecutionContext> Contexts = new();
    internal CSharpLanguageErrorTokenCatalog Catalog { get; }
    internal GuestLanguageErrorCatalog GuestCatalog { get; }
    internal IReadOnlyDictionary<string, string> OutcomeValues { get; }
    internal IReadOnlyList<GuestFunction> MemberGuards { get; }
    internal List<GuestAsyncSynchronousExceptionSite> Sites { get; } = new();
    internal HashSet<string> FailurePublishBlocks { get; } = new(StringComparer.Ordinal);

    private CSharpAsyncSynchronousExecutionContext(SemanticDocument source, SemanticLanguageErrorEffectPlan effects,
        CSharpLanguageErrorTokenCatalog catalog, GuestLanguageErrorCatalog guestCatalog)
    {
        Catalog = catalog;
        GuestCatalog = guestCatalog;
        MemberGuards = CSharpAsyncMemberAssignmentLowerer.BuildGuards(source, catalog);
        OutcomeValues = source.Callables.Where(callable => effects.OutcomeMethodIds.Contains(callable.MethodSymbolId))
            .ToDictionary(callable => CSharpGuestIds.Function(callable.MethodSymbolId), callable => callable.ReturnTypeId, StringComparer.Ordinal);
        OutcomeValues = OutcomeValues.Concat(MemberGuards.Select(guard => new KeyValuePair<string, string>(guard.Id, "type:void")))
            .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal);
    }

    internal static bool TryCreate(SemanticDocument source, SemanticLanguageErrorEffectPlan effects,
        out CSharpAsyncSynchronousExecutionContext? context)
    {
        context = null;
        if (!SemanticContract.HasAsyncSynchronousExceptions(source)
            || source.StaticInitialization is not null
            || !SemanticExceptionFlowContractValidator.IsValid(source)
            || !SemanticAsyncScopeValidator.IsValid(source)
            || source.AsyncMethods.Any(method => effects.OutcomeMethodIds.Contains(method.MethodSymbolId))) return false;
        var catalog = CSharpLanguageErrorCatalogBuilder.ForSynchronous(source, source.ExceptionFlows ?? Array.Empty<SemanticExceptionFlow>());
        if (!CSharpLanguageErrorCatalogBuilder.TryToGuest(source, catalog, out var guestCatalog, out _)) return false;
        context = new(source, effects, catalog, guestCatalog!);
        return true;
    }

    internal void Attach(SemanticDocument document)
    {
        if (!Contexts.TryGetValue(document, out _)) Contexts.Add(document, this);
    }

    internal static CSharpAsyncSynchronousExecutionContext? Find(SemanticDocument document) =>
        Contexts.TryGetValue(document, out var context) ? context : null;

    internal GuestAsyncSynchronousExceptionPlan Plan() => new(Sites
        .OrderBy(site => site.FunctionId, StringComparer.Ordinal)
        .ThenBy(site => site.CallBlockId, StringComparer.Ordinal).ToArray());
}

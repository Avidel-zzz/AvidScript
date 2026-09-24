using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public sealed record CSharpLanguageErrorCompilation(GuestModule Module);

// Compiles source-backed throw producers, local throws, direct callers, and
// bounded catch methods. The ordinary projection is private to this pass:
// the published module keeps the original exception-flow provenance.
public static class CSharpLanguageErrorCompiler
{
    public static bool TryLower(
        SemanticDocument semantic,
        string semanticSha256,
        out CSharpLanguageErrorCompilation? compilation,
        out string? error)
    {
        compilation = null;
        error = null;
        if (semantic is null || semantic.ExceptionFlows is not { Count: > 0 } flows
            || !SemanticExceptionFlowContractValidator.IsValid(semantic)
            || semantic.Diagnostics.Any(diagnostic => diagnostic.Severity == "error"
                && diagnostic.Code != "ASCS3001"))
            return Fail("Expected a validated exception-flow artifact without unrelated errors.", out error);
        SemanticExceptionFlow[] throwFlows = flows.Where(item => item.Throws.Count > 0).ToArray();
        if (throwFlows.Length == 0)
            return Fail("At least one supported throw site is required.", out error);
        SemanticExceptionFlow[] producers = throwFlows.Where(item => item.Catches.Count == 0
            && !item.Regions.Any(region => region.Kind == "finally")).ToArray();
        IReadOnlySet<string> producerMethodIds = producers.Select(item => item.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        SemanticExceptionFlow[] handlers = flows.Where(item =>
            !producerMethodIds.Contains(item.MethodSymbolId)).ToArray();
        List<SemanticControlFlowGraph> handlerGraphs = new();
        Dictionary<string, IReadOnlyList<CSharpLocalThrowSite>> localThrows = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow handler in handlers)
        {
            if (!CSharpExceptionGraphMaterializer.TryBuild(handler,
                    out SemanticControlFlowGraph? graph, out IReadOnlyList<CSharpLocalThrowSite> sites,
                    out error))
                return false;
            handlerGraphs.Add(graph!);
            if (sites.Count != 0)
                localThrows.Add(CSharpGuestIds.Function(handler.MethodSymbolId), sites);
        }
        if (throwFlows.Any(item => semantic.Callables.Count(callable =>
                    callable.MethodSymbolId == item.MethodSymbolId
                    && callable.HasBody && callable.IsStatic
                    && callable.Parameters.Count == 0
                    && callable.ReturnTypeId == "type:int32") != 1)
            || !SemanticLanguageErrorEffectPlanner.TryBuild(semantic, out var effects)
            || effects is null)
            return Fail("The exception source needs supported int32 producers and a complete direct-call effect plan.", out error);

        IReadOnlySet<string> producerIds = producers.Select(item =>
            CSharpGuestIds.Function(item.MethodSymbolId)).ToHashSet(StringComparer.Ordinal);
        IReadOnlySet<string> affected = effects.OutcomeMethodIds
            .Select(CSharpGuestIds.Function).ToHashSet(StringComparer.Ordinal);
        if (producerIds.Any(id => !affected.Contains(id)))
            return Fail("An exception producer is absent from its effect closure.", out error);
        if (!CSharpLanguageCleanupRoutePlanner.TryBuild(semantic, affected,
                out IReadOnlyDictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> plannedCleanups,
                out error))
            return false;
        Dictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> cleanupRoutes =
            new(plannedCleanups, StringComparer.Ordinal);
        foreach (SemanticExceptionFlow handler in handlers.Where(item =>
            item.Catches.Count == 0 && localThrows.TryGetValue(
                CSharpGuestIds.Function(item.MethodSymbolId), out var sites)
                && sites.Any(site => site.CleanupBlockOrdinal is not null)))
            cleanupRoutes.Add(CSharpGuestIds.Function(handler.MethodSymbolId),
                Array.Empty<CSharpLanguageCleanupRoute>());
        CSharpLanguageErrorTokenCatalog tokens = CSharpThrowProducerLowerer.BuildCatalog(flows);
        Dictionary<string, int> sourceLengths = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow item in flows)
        {
            if (sourceLengths.TryGetValue(item.SourceId, out int length)
                && length != item.SourceLength)
                return Fail("Exception flows disagree on a source unit's length.", out error);
            sourceLengths[item.SourceId] = item.SourceLength;
        }
        Dictionary<string, IReadOnlyList<CSharpLanguageCatchRoute>> catchRoutes = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow handler in handlers)
        {
            if (handler.Catches.Count == 0) continue;
            string functionId = CSharpGuestIds.Function(handler.MethodSymbolId);
            if (!affected.Contains(functionId))
                return Fail("A catch method is absent from the language-error effect closure.", out error);
            List<CSharpLanguageCatchRoute> routes = new();
            foreach (SemanticExceptionBlock block in handler.Blocks!)
            {
                List<CSharpLanguageCatchMatch> matches = new();
                foreach (CSharpLanguageErrorTypeToken type in tokens.Types)
                {
                    if (!SemanticExceptionDispatchResolver.TryResolve(semantic,
                            handler.MethodSymbolId, block.Ordinal, type.TypeId,
                            out SemanticExceptionDispatchResolution? decision)
                        || decision is null || decision.FinallyRegionOrdinals.Count != 0)
                        return Fail("Catch routing requires a validated handler without cleanup.", out error);
                    if (decision.HandlerOrdinal is { } ordinal)
                        matches.Add(new(type.Token, CSharpGuestIds.Block(handler.MethodSymbolId,
                            handler.Regions[handler.Catches[ordinal].RegionOrdinal].FirstBlockOrdinal)));
                }
                if (matches.Count != 0)
                    routes.Add(new(CSharpGuestIds.Block(handler.MethodSymbolId, block.Ordinal), matches));
            }
            catchRoutes.Add(functionId, routes);
        }
        SemanticDocument ordinary = semantic with
        {
            SchemaVersion = SemanticContract.CurrentSchemaVersion,
            SemanticVersion = SemanticContract.CurrentSemanticVersion,
            Succeeded = true,
            ExceptionFlows = null,
            ControlFlowGraphs = semantic.ControlFlowGraphs.Concat(handlerGraphs)
                .OrderBy(graph => graph.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            Diagnostics = semantic.Diagnostics.Where(diagnostic =>
                diagnostic.Code != "ASCS3001").ToArray(),
        };
        if (handlers.Length != 0
            && ordinary.Reachability?.Mode != "all_callables_compatibility")
            ordinary = ordinary with
            {
                Reachability = SemanticReachability.ExpandForExecution(
                    ordinary, effects.OutcomeMethodIds.ToArray()),
            };
        GuestFunction[] substitutes = producerIds.OrderBy(id => id, StringComparer.Ordinal)
            .Select(id => new GuestFunction(id, Array.Empty<GuestRegister>(),
                new[] { new GuestRegister("language_error:placeholder", "type:int32") },
                "type:int32", "language_error:entry", new[]
                {
                    new GuestBasicBlock("language_error:entry", new[]
                    {
                        new GuestInstruction("constant", "language_error:placeholder",
                            Array.Empty<string>(), null, null, new GuestConstant("int32", "0")),
                    }, new GuestTerminator("return", null, null, null, "language_error:placeholder")),
                })).ToArray();
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.LowerWithFunctionSubstitutes(
            ordinary, semanticSha256, substitutes);
        if (!lowered.Succeeded || lowered.Module is null)
            return Fail("The ordinary methods could not be lowered: "
                + string.Join(" | ", lowered.Diagnostics.Select(diagnostic => diagnostic.Message)), out error);
        GuestExport[] affectedExports = lowered.Module.Exports
            .Where(export => affected.Contains(export.FunctionId)).ToArray();
        Dictionary<string, GuestFunction> originalFunctions = lowered.Module.Functions
            .ToDictionary(function => function.Id, StringComparer.Ordinal);
        GuestModule internalModule = lowered.Module with
        {
            Exports = lowered.Module.Exports.Except(affectedExports).ToArray(),
        };
        if (!CSharpLanguageOutcomeRewriter.TryRewriteWithHandlers(ordinary, internalModule,
                affected, producerIds, catchRoutes, cleanupRoutes,
                out GuestModule? outcomes, out error)
            || outcomes is null)
            return false;
        Dictionary<string, GuestFunction> loweredProducers = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow item in producers)
        {
            if (!CSharpThrowProducerLowerer.TryLowerReplacing(semantic, item, outcomes,
                    out CSharpThrowProducerResult? producer, out error)
                || producer is null)
                return false;
            loweredProducers.Add(producer.Function.Id, producer.Function);
        }
        foreach (SemanticExceptionFlow item in handlers.Where(flow => flow.Throws.Count != 0))
        {
            string functionId = CSharpGuestIds.Function(item.MethodSymbolId);
            if (!CSharpLocalThrowLowerer.TryLowerReplacing(item, localThrows[functionId],
                    tokens, catchRoutes.TryGetValue(functionId, out var routes)
                        ? routes : Array.Empty<CSharpLanguageCatchRoute>(), outcomes,
                    out GuestFunction? handler, out error)
                || handler is null)
                return false;
            loweredProducers.Add(functionId, handler);
        }
        GuestModule candidate = outcomes with
        {
            SchemaVersion = GuestLanguageErrorCatalog.SchemaVersion,
            IrVersion = GuestLanguageErrorCatalog.IrVersion,
            Provenance = outcomes.Provenance with
            {
                SemanticSchemaVersion = semantic.SchemaVersion,
                SemanticVersion = semantic.SemanticVersion,
            },
            Functions = outcomes.Functions.Select(function =>
                loweredProducers.TryGetValue(function.Id, out GuestFunction? producer)
                    ? producer : function).ToArray(),
            LanguageErrorCatalog = new GuestLanguageErrorCatalog(
                tokens.Types.Select(entry =>
                    new GuestLanguageErrorTypeToken(entry.Token, entry.TypeId)).ToArray(),
                tokens.Sources.Select(entry =>
                    new GuestLanguageErrorSourceToken(entry.Token, entry.SourceId,
                        sourceLengths[entry.SourceId], entry.Span.Start, entry.Span.Length,
                        entry.Span.Line, entry.Span.Column,
                        entry.Span.EndLine, entry.Span.EndColumn)).ToArray()),
        };
        if (!CSharpLanguageErrorEntryAdapter.TryAdd(candidate, affectedExports,
                originalFunctions, out GuestModule? adapted, out error)
            || adapted is null)
            return false;
        candidate = adapted;
        GuestValidationResult validation = GuestModuleValidator.Validate(candidate);
        if (!validation.Succeeded)
            return Fail("The composed language-error module failed validation: "
                + string.Join(" | ", validation.Diagnostics.Select(item => item.Message)), out error);
        compilation = new(candidate);
        return true;
    }

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}

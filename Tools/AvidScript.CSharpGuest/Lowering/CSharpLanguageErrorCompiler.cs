using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public sealed record CSharpLanguageErrorCompilation(GuestModule Module);

// Compiles one source-backed throw producer, its direct callers, and bounded
// catch methods. The ordinary projection is private to this pass: the published
// module keeps the original exception-flow provenance.
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
        if (throwFlows.Length != 1)
            return Fail("Exactly one supported throw producer is required.", out error);
        SemanticExceptionFlow flow = throwFlows[0];
        SemanticExceptionFlow[] handlers = flows.Where(item => item != flow).ToArray();
        List<SemanticControlFlowGraph> handlerGraphs = new();
        foreach (SemanticExceptionFlow handler in handlers)
        {
            if (!CSharpExceptionGraphMaterializer.TryBuild(handler,
                    out SemanticControlFlowGraph? graph, out error))
                return false;
            handlerGraphs.Add(graph!);
        }
        SemanticCallable[] producers = semantic.Callables.Where(callable =>
            callable.MethodSymbolId == flow.MethodSymbolId).ToArray();
        if (producers.Length != 1 || !producers[0].HasBody || !producers[0].IsStatic
            || producers[0].Parameters.Count != 0
            || producers[0].ReturnTypeId != "type:int32"
            || !SemanticLanguageErrorEffectPlanner.TryBuild(semantic, out var effects)
            || effects is null)
            return Fail("The exception source needs a supported int32 producer and a complete direct-call effect plan.", out error);

        string producerId = CSharpGuestIds.Function(flow.MethodSymbolId);
        IReadOnlySet<string> affected = effects.OutcomeMethodIds
            .Select(CSharpGuestIds.Function).ToHashSet(StringComparer.Ordinal);
        if (!affected.Contains(producerId))
            return Fail("The exception producer is absent from its effect closure.", out error);
        CSharpLanguageErrorTokenCatalog tokens = CSharpThrowProducerLowerer.BuildCatalog(flows);
        Dictionary<string, IReadOnlyList<CSharpLanguageCatchRoute>> catchRoutes = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow handler in handlers)
        {
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
        GuestFunction substitute = new(producerId, Array.Empty<GuestRegister>(),
            new[] { new GuestRegister("language_error:placeholder", "type:int32") },
            "type:int32", "language_error:entry", new[]
            {
                new GuestBasicBlock("language_error:entry", new[]
                {
                    new GuestInstruction("constant", "language_error:placeholder",
                        Array.Empty<string>(), null, null, new GuestConstant("int32", "0")),
                }, new GuestTerminator("return", null, null, null, "language_error:placeholder")),
            });
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.LowerWithFunctionSubstitutes(
            ordinary, semanticSha256, new[] { substitute });
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
                affected, new[] { producerId }.ToHashSet(StringComparer.Ordinal), catchRoutes,
                out GuestModule? outcomes, out error)
            || outcomes is null)
            return false;
        if (!CSharpThrowProducerLowerer.TryLowerReplacing(semantic, flow, outcomes,
                out CSharpThrowProducerResult? producer, out error)
            || producer is null)
            return false;
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
                function.Id == producerId ? producer.Function : function).ToArray(),
            LanguageErrorCatalog = new GuestLanguageErrorCatalog(
                producer.Catalog.Types.Select(entry =>
                    new GuestLanguageErrorTypeToken(entry.Token, entry.TypeId)).ToArray(),
                producer.Catalog.Sources.Select(entry =>
                    new GuestLanguageErrorSourceToken(entry.Token, entry.SourceId,
                        flow.SourceLength, entry.Span.Start, entry.Span.Length,
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

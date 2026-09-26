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
        bool hasCatchVariables = flows.Any(flow => flow.Catches.Any(handler =>
            handler.ExceptionVariableSymbolId is not null));
        if (flows.Any(flow => flow.Catches.Any(handler =>
                handler.ExceptionVariableSymbolId is { } id
                && (handler.ExceptionTypeId != CSharpThrowProducerLowerer.ExceptionTypeId
                    || semantic.Symbols.Count(symbol => symbol.Id == id
                        && symbol.Kind == "local"
                        && symbol.TypeId == handler.ExceptionTypeId
                        && symbol.ContainingSymbolId == flow.MethodSymbolId) != 1)))
            || flows.SelectMany(flow => flow.Catches)
                .Where(handler => handler.ExceptionVariableSymbolId is not null)
                .GroupBy(handler => handler.ExceptionVariableSymbolId,
                    StringComparer.Ordinal).Any(group => group.Count() != 1))
            return Fail("A catch variable needs a unique System.Exception local symbol.", out error);
        SemanticExceptionFlow[] throwFlows = flows.Where(item => item.Throws.Count > 0).ToArray();
        if (throwFlows.Length == 0)
            return Fail("At least one supported throw site is required.", out error);
        // The standalone producer only models an unconditional, argument-free
        // throw. Conditional guards and parameterized methods retain their CFG.
        SemanticExceptionFlow[] producers = throwFlows.Where(item => item.Catches.Count == 0
            && !item.Regions.Any(region => region.Kind == "finally")
            && item.Throws.Count == 1 && item.Blocks is { Count: 3 }
            && item.Blocks.All(block => block.Operations.Count == 0)
            && semantic.Callables.Any(callable => callable.MethodSymbolId == item.MethodSymbolId
                && callable.IsStatic && callable.Parameters.Count == 0
                && callable.ReturnTypeId is "type:int32" or "type:void")).ToArray();
        IReadOnlySet<string> producerMethodIds = producers.Select(item => item.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        SemanticExceptionFlow[] handlers = flows.Where(item =>
            !producerMethodIds.Contains(item.MethodSymbolId)).ToArray();
        List<SemanticControlFlowGraph> handlerGraphs = new();
        Dictionary<string, IReadOnlyList<CSharpLocalThrowSite>> localThrows = new(StringComparer.Ordinal);
        Dictionary<string, IReadOnlyList<CSharpNormalReturnCleanupSite>> normalReturns =
            new(StringComparer.Ordinal);
        Dictionary<string, IReadOnlyList<CSharpRethrowSite>> rethrows = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow handler in handlers)
        {
            SemanticCallable[] callables = semantic.Callables.Where(callable =>
                callable.MethodSymbolId == handler.MethodSymbolId).Take(2).ToArray();
            if (callables.Length != 1)
                return Fail("An exception method needs one source-backed callable.", out error);
            if (!CSharpExceptionGraphMaterializer.TryBuild(handler,
                    callables[0].ReturnTypeId,
                    out SemanticControlFlowGraph? graph, out IReadOnlyList<CSharpLocalThrowSite> sites,
                    out IReadOnlyList<CSharpNormalReturnCleanupSite> returnSites,
                    out IReadOnlyList<CSharpRethrowSite> rethrowSites,
                    out error))
                return false;
            handlerGraphs.Add(graph!);
            if (sites.Count != 0)
                localThrows.Add(CSharpGuestIds.Function(handler.MethodSymbolId), sites);
            if (returnSites.Count != 0)
                normalReturns.Add(CSharpGuestIds.Function(handler.MethodSymbolId), returnSites);
            if (rethrowSites.Count != 0)
                rethrows.Add(CSharpGuestIds.Function(handler.MethodSymbolId), rethrowSites);
        }
        if (throwFlows.Any(item => semantic.Callables.Count(callable =>
                    callable.MethodSymbolId == item.MethodSymbolId
                    && callable.HasBody && !callable.IsConstructor
                    && (callable.ReturnTypeId is "type:int32" or "type:void"
                        || CSharpReferenceObjects.Types(semantic).Contains(callable.ReturnTypeId))) != 1)
            || !SemanticLanguageErrorEffectPlanner.TryBuild(semantic, out var effects)
            || effects is null)
            return Fail("The exception source needs supported int32, void or source-class methods and a complete direct-call effect plan.", out error);

        IReadOnlySet<string> producerIds = producers.Select(item =>
            CSharpGuestIds.Function(item.MethodSymbolId)).ToHashSet(StringComparer.Ordinal);
        IReadOnlySet<string> affected = effects.OutcomeMethodIds
            .Select(CSharpGuestIds.Function).ToHashSet(StringComparer.Ordinal);
        if (producerIds.Any(id => !affected.Contains(id)))
            return Fail("An exception producer is absent from its effect closure.", out error);
        bool combinedTaskContract = semantic.SchemaVersion == SemanticContract.TaskLanguageErrorSchemaVersion
            && semantic.SemanticVersion == SemanticContract.TaskLanguageErrorSemanticVersion;
        if (combinedTaskContract && semantic.AsyncMethods.Any(method =>
            effects.OutcomeMethodIds.Contains(method.MethodSymbolId)))
            return Fail("An async Task method cannot enter the synchronous language-error effect closure.", out error);
        if (!CSharpLanguageCleanupRoutePlanner.TryBuild(semantic, affected,
                out IReadOnlyDictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> plannedCleanups,
                out error))
            return false;
        Dictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> cleanupRoutes =
            new(plannedCleanups, StringComparer.Ordinal);
        foreach (SemanticExceptionFlow handler in handlers.Where(item =>
            normalReturns.ContainsKey(CSharpGuestIds.Function(item.MethodSymbolId))
            || rethrows.TryGetValue(CSharpGuestIds.Function(item.MethodSymbolId),
                out var rethrowSites) && rethrowSites.Any(site =>
                    site.CleanupBlockOrdinal is not null)
            || item.Catches.Count == 0 && localThrows.TryGetValue(
                CSharpGuestIds.Function(item.MethodSymbolId), out var sites)
                && sites.Any(site => site.CleanupBlockOrdinals is { Count: > 0 }
                    || site.BranchingCleanup is not null)))
        {
            string functionId = CSharpGuestIds.Function(handler.MethodSymbolId);
            IEnumerable<CSharpLanguageCleanupRoute> returnRoutes =
                normalReturns.TryGetValue(functionId, out var returnSites)
                    ? returnSites.Select(site => new CSharpLanguageCleanupRoute(
                        CSharpGuestIds.Block(handler.MethodSymbolId, site.BlockOrdinal),
                        new[] { new CSharpLanguageCleanupRegion(
                            (site.BranchingCleanup?.BlockOrdinals
                                ?? new[] { site.CleanupBlockOrdinal })
                            .Select(ordinal => CSharpGuestIds.Block(
                                handler.MethodSymbolId, ordinal)).ToArray()) })).ToArray()
                    : Array.Empty<CSharpLanguageCleanupRoute>();
            IEnumerable<CSharpLanguageCleanupRoute> rethrowRoutes =
                rethrows.TryGetValue(functionId, out var rethrowSites)
                    ? rethrowSites.Where(site => site.CleanupBlockOrdinal is not null)
                        .Select(site => new CSharpLanguageCleanupRoute(
                            CSharpGuestIds.Block(handler.MethodSymbolId, site.BlockOrdinal),
                            new[] { new CSharpLanguageCleanupRegion(
                                (site.BranchingCleanup?.BlockOrdinals
                                    ?? new[] { site.CleanupBlockOrdinal!.Value })
                                    .Select(ordinal => CSharpGuestIds.Block(
                                        handler.MethodSymbolId, ordinal)).ToArray()) }))
                        .ToArray()
                    : Array.Empty<CSharpLanguageCleanupRoute>();
            cleanupRoutes.Add(functionId, returnRoutes.Concat(rethrowRoutes).ToArray());
        }
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
            string functionId = CSharpGuestIds.Function(handler.MethodSymbolId);
            if (handler.Catches.Count == 0)
            {
                // This empty route certifies that the source-backed forward CFG
                // was materialized even though it has no local catch target.
                if (handler.Throws.Count != 0
                    && !handler.Regions.Any(region => region.Kind == "finally"))
                    catchRoutes.Add(functionId, Array.Empty<CSharpLanguageCatchRoute>());
                continue;
            }
            if (!affected.Contains(functionId))
                return Fail("A catch method is absent from the language-error effect closure.", out error);
            IReadOnlySet<int> rethrowHandlers = rethrows.TryGetValue(functionId, out var rethrowSites)
                ? rethrowSites.Select(site => site.HandlerOrdinal).ToHashSet()
                : new HashSet<int>();
            List<CSharpLanguageCatchRoute> routes = new();
            foreach (SemanticExceptionBlock block in handler.Blocks!)
            {
                List<CSharpLanguageCatchMatch> matches = new();
                foreach (CSharpLanguageErrorTypeToken type in tokens.Types)
                {
                    if (!SemanticExceptionDispatchResolver.TryResolve(semantic,
                            handler.MethodSymbolId, block.Ordinal, type.TypeId,
                            out SemanticExceptionDispatchResolution? decision)
                        || decision is null)
                        return Fail("Catch routing requires a validated cleanup and handler path.", out error);
                    if (decision.FinallyRegionOrdinals.Count != 0)
                    {
                        string sourceBlockId = CSharpGuestIds.Block(handler.MethodSymbolId,
                            block.Ordinal);
                        string[][] cleanupRegionBlocks = decision.FinallyRegionOrdinals
                            .Select(region => Enumerable.Range(
                                    handler.Regions[region].FirstBlockOrdinal,
                                    handler.Regions[region].LastBlockOrdinal
                                        - handler.Regions[region].FirstBlockOrdinal + 1)
                                .Select(ordinal => CSharpGuestIds.Block(
                                    handler.MethodSymbolId, ordinal)).ToArray())
                            .ToArray();
                        if (decision.HandlerOrdinal is not null
                            || !cleanupRoutes.TryGetValue(functionId, out var functionCleanups)
                            || !functionCleanups.Any(route => route.SourceBlockId == sourceBlockId
                                && route.Regions.Count == cleanupRegionBlocks.Length
                                && route.Regions.Select((region, index) =>
                                        region.BlockIds.SequenceEqual(cleanupRegionBlocks[index]))
                                    .All(matches => matches)))
                            return Fail("Catch routing requires a validated cleanup and handler path.", out error);
                    }
                    if (decision.HandlerOrdinal is { } ordinal)
                        matches.Add(new(type.Token, CSharpGuestIds.Block(handler.MethodSymbolId,
                            handler.Regions[handler.Catches[ordinal].RegionOrdinal].FirstBlockOrdinal),
                            rethrowHandlers.Contains(ordinal)
                            || handler.Catches[ordinal].ExceptionVariableSymbolId is not null));
                }
                if (matches.Count != 0)
                    routes.Add(new(CSharpGuestIds.Block(handler.MethodSymbolId, block.Ordinal), matches));
            }
            catchRoutes.Add(functionId, routes);
        }
        SemanticDocument ordinary = semantic with
        {
            SchemaVersion = combinedTaskContract ? OrdinaryTaskSchema(semantic)
                : SemanticContract.CurrentSchemaVersion,
            SemanticVersion = combinedTaskContract ? OrdinaryTaskVersion(semantic)
                : SemanticContract.CurrentSemanticVersion,
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
            .Select(id => CreateProducerSubstitute(id, semantic.Callables.Single(callable =>
                CSharpGuestIds.Function(callable.MethodSymbolId) == id).ReturnTypeId)).ToArray();
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.LowerWithFunctionSubstitutes(
            ordinary, semanticSha256, substitutes, hasCatchVariables);
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
                out GuestModule? outcomes, out error, combinedTaskContract)
            || outcomes is null)
            return false;
        if (hasCatchVariables)
        {
            if (!TryBindCatchVariables(flows, outcomes,
                    out GuestModule? bound, out error) || bound is null)
                return false;
            outcomes = bound;
        }
        Dictionary<string, GuestFunction> loweredProducers = new(StringComparer.Ordinal);
        foreach (SemanticExceptionFlow item in producers)
        {
            if (!CSharpThrowProducerLowerer.TryLowerReplacing(semantic, item, outcomes,
                    out CSharpThrowProducerResult? producer, out error,
                    combinedTaskContract)
                || producer is null)
                return false;
            loweredProducers.Add(producer.Function.Id, producer.Function);
        }
        foreach (SemanticExceptionFlow item in handlers.Where(flow => flow.Throws.Count != 0
            || normalReturns.ContainsKey(CSharpGuestIds.Function(flow.MethodSymbolId))))
        {
            string functionId = CSharpGuestIds.Function(item.MethodSymbolId);
            if (!CSharpLocalThrowLowerer.TryLowerReplacing(item,
                    localThrows.TryGetValue(functionId, out var sites)
                        ? sites : Array.Empty<CSharpLocalThrowSite>(),
                    normalReturns.TryGetValue(functionId, out var returnSites)
                        ? returnSites : Array.Empty<CSharpNormalReturnCleanupSite>(),
                    rethrows.TryGetValue(functionId, out var rethrowSites)
                        ? rethrowSites : Array.Empty<CSharpRethrowSite>(),
                    tokens, catchRoutes.TryGetValue(functionId, out var routes)
                        ? routes : Array.Empty<CSharpLanguageCatchRoute>(), outcomes,
                    out GuestFunction? handler, out error)
                || handler is null)
                return false;
            loweredProducers.Add(functionId, handler);
        }
        GuestModule candidate = outcomes with
        {
            SchemaVersion = combinedTaskContract ? 20 : GuestLanguageErrorCatalog.SchemaVersion,
            IrVersion = combinedTaskContract ? "1.19" : GuestLanguageErrorCatalog.IrVersion,
            Provenance = outcomes.Provenance with
            {
                SemanticSchemaVersion = semantic.SchemaVersion,
                SemanticVersion = semantic.SemanticVersion,
            },
            Functions = outcomes.Functions.Select(function =>
                loweredProducers.TryGetValue(function.Id, out GuestFunction? producer)
                    ? producer : function).ToArray(),
            Imports = combinedTaskContract ? outcomes.Imports
                .Concat(outcomes.Imports.Any(import =>
                    import.Id == CSharpTaskResultAbi.RetainForContinuationImportId)
                    ? Array.Empty<GuestImport>()
                    : new[] { CSharpTaskResultAbi.RetainForContinuationImport() })
                .Append(CSharpTaskResultAbi.FaultLanguageErrorImport())
                .Append(CSharpTaskResultAbi.LanguageErrorMetaImport())
                .Append(CSharpTaskResultAbi.LanguageErrorRootImport()).ToArray()
                : outcomes.Imports,
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

    private static int OrdinaryTaskSchema(SemanticDocument document)
    {
        IReadOnlyList<SemanticAsyncMethod> methods = document.AsyncMethods;
        if (methods.Any(method => method.TaskLocalSymbolIds is not null))
            return SemanticContract.TaskAliasSchemaVersion;
        if (methods.Any(method => method.Segments.Any(segment =>
                segment.AwaitSite?.ResultStorageKind == "existing_local")))
            return SemanticContract.TaskExistingLocalSchemaVersion;
        if (methods.Any(method => method.Segments.Any(segment =>
                segment.AwaitSite?.ResultStorageKind == "static_field")))
            return SemanticContract.TaskAssignmentSchemaVersion;
        if (methods.Any(method => method.Segments.Any(segment =>
                segment.AwaitSite?.TaskLocalSymbolId is not null)))
            return SemanticContract.TaskLocalSchemaVersion;
        return SemanticContract.TaskResultSchemaVersion;
    }

    private static string OrdinaryTaskVersion(SemanticDocument document) =>
        OrdinaryTaskSchema(document) switch
        {
            SemanticContract.TaskAliasSchemaVersion => SemanticContract.TaskAliasSemanticVersion,
            SemanticContract.TaskExistingLocalSchemaVersion => SemanticContract.TaskExistingLocalSemanticVersion,
            SemanticContract.TaskAssignmentSchemaVersion => SemanticContract.TaskAssignmentSemanticVersion,
            SemanticContract.TaskLocalSchemaVersion => SemanticContract.TaskLocalSemanticVersion,
            _ => SemanticContract.TaskResultSemanticVersion,
        };

    private static GuestFunction CreateProducerSubstitute(string id, string returnTypeId)
    {
        bool returnsVoid = returnTypeId == "type:void";
        const string entry = "language_error:entry";
        const string placeholder = "language_error:placeholder";
        return new GuestFunction(id, Array.Empty<GuestRegister>(),
            returnsVoid ? Array.Empty<GuestRegister>()
                : new[] { new GuestRegister(placeholder, returnTypeId) },
            returnTypeId, entry, new[]
            {
                new GuestBasicBlock(entry,
                    returnsVoid ? Array.Empty<GuestInstruction>()
                        : new[] { new GuestInstruction("constant", placeholder,
                            Array.Empty<string>(), null, null, new GuestConstant("int32", "0")) },
                    new GuestTerminator("return", null, null, null,
                        returnsVoid ? null : placeholder)),
            });
    }

    private static bool TryBindCatchVariables(
        IReadOnlyList<SemanticExceptionFlow> flows,
        GuestModule module,
        out GuestModule? bound,
        out string? error)
    {
        bound = null;
        error = null;
        Dictionary<string, GuestFunction> functions = module.Functions.ToDictionary(
            function => function.Id, StringComparer.Ordinal);
        foreach (SemanticExceptionFlow flow in flows.Where(item => item.Catches.Any(
            handler => handler.ExceptionVariableSymbolId is not null)))
        {
            string functionId = CSharpGuestIds.Function(flow.MethodSymbolId);
            if (!functions.TryGetValue(functionId, out GuestFunction? function))
                return Fail("The catch variable has no lowered owner function.", out error);
            List<GuestRegister> locals = function.Locals.ToList();
            HashSet<string> registerIds = function.Parameters.Concat(locals)
                .Select(register => register.Id).ToHashSet(StringComparer.Ordinal);
            Dictionary<string, GuestBasicBlock> blocks = function.Blocks.ToDictionary(
                block => block.Id, StringComparer.Ordinal);
            foreach (SemanticCatchHandler handler in flow.Catches.Where(item =>
                item.ExceptionVariableSymbolId is not null))
            {
                int blockOrdinal = flow.Regions[handler.RegionOrdinal].FirstBlockOrdinal;
                // A single-block rethrow has no readable use of the catch local.
                // Its captured outcome already owns the original error root.
                if (flow.Branches.Any(branch => branch.SourceBlockOrdinal == blockOrdinal
                    && branch.Semantics == "rethrow"))
                    continue;
                string blockId = CSharpGuestIds.Block(flow.MethodSymbolId, blockOrdinal);
                string capture = CSharpLanguageCatchContext.OutcomeRegister(blockId);
                string storage = CSharpGuestIds.Local(handler.ExceptionVariableSymbolId!);
                string root = "language_catch:bind_root:" + handler.Ordinal;
                string reference = "language_catch:bind_ref:" + handler.Ordinal;
                if (!blocks.TryGetValue(blockId, out GuestBasicBlock? block)
                    || !locals.Any(register => register.Id == capture
                        && register.TypeId == function.ReturnTypeId)
                    || !locals.Any(register => register.Id == storage
                        && register.TypeId == CSharpThrowProducerLowerer.ExceptionTypeId)
                    || !registerIds.Add(root) || !registerIds.Add(reference))
                    return Fail("The catch variable has no unique captured error root or local storage.", out error);
                locals.Add(new GuestRegister(root, "type:language_error_root"));
                locals.Add(new GuestRegister(reference,
                    CSharpThrowProducerLowerer.ExceptionTypeId));
                GuestInstruction[] binding =
                {
                    new("field_load", root, new[] { capture }, "field:error_root", null, null),
                    new("managed_cast", reference, new[] { root }, null, null, null),
                    new("local_store", null, new[] { reference }, storage, null, null),
                };
                blocks[blockId] = block with
                {
                    Instructions = binding.Concat(block.Instructions).ToArray(),
                };
            }
            functions[functionId] = function with
            {
                Locals = locals,
                Blocks = function.Blocks.Select(block => blocks[block.Id]).ToArray(),
            };
        }
        GuestModule candidate = module with
        {
            Functions = module.Functions.Select(function => functions[function.Id]).ToArray(),
        };
        GuestValidationResult validation = GuestModuleValidator.Validate(candidate);
        if (!validation.Succeeded)
            return Fail("The catch variable binding failed Guest validation: "
                + string.Join(" | ", validation.Diagnostics.Select(item => item.Message)), out error);
        bound = candidate;
        return true;
    }

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}

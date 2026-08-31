using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpSemanticInputValidator
{
    private const int CompilerCallbackIdStart = 0x40000000;
    private const int MaximumAsyncAwaitsPerMethod = 16;
    private const int MaximumAsyncAwaitsPerModule = 64;
    private const int MaximumAsyncStateSlotsPerAwait = 64;

    private static readonly string[] ObjectContinuationParameterTypeIds =
    {
        "type:global::AvidScript.AvidContinuationStatus",
        "type:global::AvidScript.AvidLoadedObject",
    };

    private sealed record GameplayCallbackContract(
        string Name,
        IReadOnlyList<string> ParameterTypeIds);

    private static readonly IReadOnlyDictionary<int, GameplayCallbackContract> GameplayCallbackContracts =
        new Dictionary<int, GameplayCallbackContract>
        {
            [1] = new("OnBeginOverlap", new[]
            {
                "type:global::AvidScript.AActor",
                "type:global::AvidScript.FVector",
            }),
            [2] = new("OnEndOverlap", new[]
            {
                "type:global::AvidScript.AActor",
                "type:global::AvidScript.FVector",
            }),
            [3] = new("OnHit", new[]
            {
                "type:global::AvidScript.AActor",
                "type:global::AvidScript.FVector",
            }),
            [4] = new("OnInput", new[] { "type:global::AvidScript.InputEvent" }),
        };

    public static bool IsValid(SemanticDocument document)
    {
        if (document.Source is null
            || document.Types is null
            || document.TypeShapes is null
            || document.Symbols is null
            || document.Callables is null
            || document.Methods is null
            || document.ControlFlowGraphs is null
            || document.GameplayEventCallbacks is null
            || document.DelegateEventCallbacks is null
            || document.ContinuationCallbacks is null
            || document.AsyncMethods is null
            || document.UeTypeDeclarations is null
            || document.Diagnostics is null
            || string.IsNullOrWhiteSpace(document.Language)
            || string.IsNullOrWhiteSpace(document.SemanticVersion)
            || !IsSupportedContract(document.SchemaVersion, document.SemanticVersion)
            || string.IsNullOrWhiteSpace(document.Source.SourceId)
            || document.Source.Sha256 is null
            || document.Source.FrontendSha256 is null)
        {
            return false;
        }

        return ValidateTypes(document.Types)
            && ValidateTypeShapes(document.TypeShapes)
            && ValidateSymbols(document.Symbols)
            && ValidateCallables(document.SchemaVersion, document.Callables)
            && SemanticUeTypeContractValidator.TryValidate(document, out _)
            && ValidateGameplayEventCallbacks(document)
            && ValidateDelegateEventCallbacks(document)
            && ValidateContinuationCallbacks(document)
            && ValidateAsyncMethods(document)
            && ValidateMethods(document.Methods)
            && ValidateReachability(document)
            && ValidateGraphs(document.ControlFlowGraphs)
            && document.Diagnostics.All(diagnostic => diagnostic is not null
                && !string.IsNullOrWhiteSpace(diagnostic.Code)
                && !string.IsNullOrWhiteSpace(diagnostic.Severity)
                && diagnostic.Message is not null
                && diagnostic.Span is not null);
    }

    private static bool ValidateTypes(IReadOnlyList<SemanticType> types)
    {
        return types.All(type => type is not null
                && !string.IsNullOrWhiteSpace(type.Id)
                && !string.IsNullOrWhiteSpace(type.CanonicalName)
                && !string.IsNullOrWhiteSpace(type.DisplayName)
                && !string.IsNullOrWhiteSpace(type.Kind))
            && Unique(types.Select(type => type.Id));
    }

    private static bool ValidateTypeShapes(IReadOnlyList<SemanticTypeShape> shapes)
    {
        return shapes.All(shape => shape is not null
                && !string.IsNullOrWhiteSpace(shape.TypeId)
                && new[]
                {
                    shape.ElementTypeId,
                    shape.EnumUnderlyingTypeId,
                    shape.GenericArgumentTypeId,
                }.Count(value => value is not null) <= 1)
            && Unique(shapes.Select(shape => shape.TypeId));
    }

    private static bool ValidateSymbols(IReadOnlyList<SemanticSymbol> symbols)
    {
        return symbols.All(symbol => symbol is not null
                && !string.IsNullOrWhiteSpace(symbol.Id)
                && !string.IsNullOrWhiteSpace(symbol.Kind)
                && symbol.Name is not null
                && symbol.Signature is not null
                && !string.IsNullOrWhiteSpace(symbol.Accessibility)
                && symbol.Span is not null)
            && Unique(symbols.Select(symbol => symbol.Id))
            && Unique(symbols
                .Where(symbol => symbol.Kind == "field" && symbol.ContainingSymbolId is not null)
                .Select(symbol => symbol.ContainingSymbolId + "\n" + symbol.Name));
    }

    private static bool ValidateCallables(
        int schemaVersion,
        IReadOnlyList<SemanticCallable> callables)
    {
        if (callables.Any(callable => callable is null)
            || !Unique(callables.Select(callable => callable.MethodSymbolId)))
        {
            return false;
        }

        HashSet<string> exportNames = new(StringComparer.Ordinal);
        foreach (SemanticCallable callable in callables)
        {
            if (callable is null
                || string.IsNullOrWhiteSpace(callable.MethodSymbolId)
                || string.IsNullOrWhiteSpace(callable.ContainingTypeId)
                || string.IsNullOrWhiteSpace(callable.ReturnTypeId)
                || callable.Parameters is null
                || callable.Parameters.Any(parameter => parameter is null)
                || !Unique(callable.Parameters.Select(parameter => parameter.SymbolId))
                || callable.Parameters.Select(parameter => parameter.Ordinal).Distinct().Count()
                    != callable.Parameters.Count
                || callable.Parameters.Any(parameter => parameter is null
                    || parameter.Ordinal < 0
                    || string.IsNullOrWhiteSpace(parameter.SymbolId)
                    || parameter.Name is null
                    || string.IsNullOrWhiteSpace(parameter.TypeId)
                    || parameter.RefKind is not ("none" or "ref" or "out" or "in"))
                || (callable.Import is not null
                    && (string.IsNullOrWhiteSpace(callable.Import.Module)
                        || string.IsNullOrWhiteSpace(callable.Import.Name)))
                || (callable.Export is not null
                    && (string.IsNullOrWhiteSpace(callable.Export.Name)
                        || !exportNames.Add(callable.Export.Name)))
                || (schemaVersion < 8 && callable.Optimization is not null)
                || (schemaVersion >= 8
                    && callable.Optimization is not null
                    && (callable.Optimization.OptimizationClass is not
                            ("none" or "snapshot_read" or "buffered_write" or "fused_call")
                        || (callable.Optimization.OptimizationClass != "none"
                            && callable.Optimization.BindingOrdinal < 0))))
            {
                return false;
            }
        }

        return true;
    }

    private static bool ValidateReachability(SemanticDocument document)
    {
        if (document.SchemaVersion < 5)
        {
            return true;
        }

        SemanticReachability? reachability = document.Reachability;
        if (reachability is null
            || reachability.Mode is not ("export_roots" or "entrypoint_roots" or "all_callables_compatibility")
            || reachability.RootCallableIds is null
            || reachability.ReachableCallableIds is null
            || reachability.ReachableImports is null
            || !Unique(reachability.RootCallableIds)
            || !Unique(reachability.ReachableCallableIds)
            || !IsOrdinalSorted(reachability.RootCallableIds)
            || !IsOrdinalSorted(reachability.ReachableCallableIds))
        {
            return false;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        HashSet<string> callbackIds = document.GameplayEventCallbacks
            .Select(callback => callback.MethodSymbolId)
            .Concat(document.DelegateEventCallbacks.Select(callback => callback.MethodSymbolId))
            .Concat(document.ContinuationCallbacks.Select(callback => callback.MethodSymbolId))
            .ToHashSet(StringComparer.Ordinal);
        HashSet<string> ueTypeMethodIds = document.UeTypeDeclarations
            .SelectMany(type => type.Functions)
            .Where(function => !function.Flags.Contains("blueprint_implementable_event"))
            .Select(function => function.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        string[] expectedRootIds = document.Callables
            .Where(callable => callable.Export is not null)
            .Select(callable => callable.MethodSymbolId)
            .Concat(callbackIds)
            .Concat(ueTypeMethodIds)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToArray();
        string expectedMode = callbackIds.Count != 0
            ? "entrypoint_roots"
            : expectedRootIds.Length != 0
                ? "export_roots"
                : "all_callables_compatibility";
        HashSet<string> reachableIds = reachability.ReachableCallableIds.ToHashSet(StringComparer.Ordinal);
        if (!string.Equals(reachability.Mode, expectedMode, StringComparison.Ordinal)
            || reachability.RootCallableIds.Any(id =>
                !reachableIds.Contains(id)
                || !callablesById.TryGetValue(id, out SemanticCallable? callable)
                || (callable.Export is null
                    && !callbackIds.Contains(id)
                    && !ueTypeMethodIds.Contains(id)))
            || reachableIds.Any(id => !callablesById.ContainsKey(id))
            || (reachability.Mode is "export_roots" or "entrypoint_roots"
                && !reachability.RootCallableIds.SequenceEqual(expectedRootIds))
            || (reachability.Mode == "all_callables_compatibility"
                && (reachability.RootCallableIds.Count != 0
                    || reachableIds.Count != callablesById.Count)))
        {
            return false;
        }

        if (document.SchemaVersion >= 12
            && document.AsyncMethods
                .SelectMany(method => method.Segments)
                .SelectMany(segment => segment.Statements
                    .Select(statement => statement.Operation)
                    .Concat(segment.AwaitSite?.Arguments ?? Array.Empty<SemanticOperation>()))
                .SelectMany(EnumerateOperations)
                .SelectMany(operation => new[]
                {
                    operation.SymbolId,
                    operation.Conversion?.MethodSymbolId,
                    operation.InputConversion?.MethodSymbolId,
                    operation.OutputConversion?.MethodSymbolId,
                })
                .Where(id => id is not null && callablesById.ContainsKey(id))
                .Any(id => !reachableIds.Contains(id!)))
        {
            return false;
        }

        SemanticReachableImport[] expectedImports = reachability.ReachableCallableIds
            .Select(id => callablesById[id])
            .Where(callable => callable.Import is not null)
            .Select(callable => new SemanticReachableImport(
                callable.MethodSymbolId,
                callable.Import!.Module,
                callable.Import.Name))
            .OrderBy(import => import.Module, StringComparer.Ordinal)
            .ThenBy(import => import.Name, StringComparer.Ordinal)
            .ThenBy(import => import.MethodSymbolId, StringComparer.Ordinal)
            .ToArray();
        return reachability.ReachableImports.All(import => import is not null)
            && reachability.ReachableImports.SequenceEqual(expectedImports);
    }

    private static bool ValidateGameplayEventCallbacks(SemanticDocument document)
    {
        if (document.SchemaVersion < 7)
        {
            return document.GameplayEventCallbacks.Count == 0;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        Dictionary<string, SemanticSymbol> symbolsById = document.Symbols.ToDictionary(
            symbol => symbol.Id,
            StringComparer.Ordinal);
        return document.GameplayEventCallbacks.All(callback => callback is not null
                && GameplayCallbackContracts.TryGetValue(
                    callback.EventType,
                    out GameplayCallbackContract? contract)
                && string.Equals(callback.Name, contract.Name, StringComparison.Ordinal)
                && !string.IsNullOrWhiteSpace(callback.MethodSymbolId)
                && IsValidCallbackSpan(callback.Span, callback.Name, document.Source.Length)
                && callablesById.TryGetValue(callback.MethodSymbolId, out SemanticCallable? callable)
                && callable.HasBody
                && callable.IsStatic
                && !callable.IsConstructor
                && callable.Import is null
                && string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
                && ParametersMatch(callable.Parameters, contract.ParameterTypeIds)
                && symbolsById.TryGetValue(callback.MethodSymbolId, out SemanticSymbol? symbol)
                && string.Equals(symbol.Kind, "method", StringComparison.Ordinal)
                && string.Equals(symbol.Name, callback.Name, StringComparison.Ordinal)
                && symbol.IsStatic
                && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
                && string.Equals(symbol.TypeId, "type:void", StringComparison.Ordinal)
                && string.Equals(
                    symbol.ContainingSymbolId,
                    "symbol:" + callable.ContainingTypeId,
                    StringComparison.Ordinal)
                && Contains(symbol.Span, callback.Span))
            && document.GameplayEventCallbacks
                .Select(callback => callback.EventType)
                .SequenceEqual(document.GameplayEventCallbacks
                    .Select(callback => callback.EventType)
                    .OrderBy(eventType => eventType))
            && document.GameplayEventCallbacks
                .Select(callback => callback.EventType)
                .Distinct()
                .Count() == document.GameplayEventCallbacks.Count
            && Unique(document.GameplayEventCallbacks.Select(callback => callback.MethodSymbolId));
    }

    private static bool ValidateDelegateEventCallbacks(SemanticDocument document)
    {
        if (document.SchemaVersion < 9)
        {
            return document.DelegateEventCallbacks.Count == 0;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        Dictionary<string, SemanticSymbol> symbolsById = document.Symbols.ToDictionary(
            symbol => symbol.Id,
            StringComparer.Ordinal);
        return document.DelegateEventCallbacks.All(callback => callback is not null
                && IsSha256(callback.SubscriptionId)
                && string.Equals(
                    callback.ExportName,
                    "avid_on_delegate_" + callback.SubscriptionId[..16],
                    StringComparison.Ordinal)
                && !string.IsNullOrWhiteSpace(callback.Name)
                && !string.IsNullOrWhiteSpace(callback.MethodSymbolId)
                && IsValidCallbackSpan(callback.Span, callback.Name, document.Source.Length)
                && callablesById.TryGetValue(callback.MethodSymbolId, out SemanticCallable? callable)
                && callable.HasBody
                && callable.IsStatic
                && !callable.IsConstructor
                && callable.Import is null
                && callable.Parameters.All(parameter => parameter.RefKind is "none" or "ref" or "out")
                && symbolsById.TryGetValue(callback.MethodSymbolId, out SemanticSymbol? symbol)
                && string.Equals(symbol.Kind, "method", StringComparison.Ordinal)
                && string.Equals(symbol.Name, callback.Name, StringComparison.Ordinal)
                && symbol.IsStatic
                && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
                && string.Equals(symbol.TypeId, callable.ReturnTypeId, StringComparison.Ordinal)
                && string.Equals(
                    symbol.ContainingSymbolId,
                    "symbol:" + callable.ContainingTypeId,
                    StringComparison.Ordinal)
                && Contains(symbol.Span, callback.Span))
            && document.DelegateEventCallbacks
                .Select(callback => callback.SubscriptionId)
                .SequenceEqual(document.DelegateEventCallbacks
                    .Select(callback => callback.SubscriptionId)
                    .OrderBy(subscriptionId => subscriptionId, StringComparer.Ordinal))
            && Unique(document.DelegateEventCallbacks.Select(callback => callback.SubscriptionId))
            && Unique(document.DelegateEventCallbacks.Select(callback => callback.ExportName))
            && Unique(document.DelegateEventCallbacks.Select(callback => callback.MethodSymbolId));
    }

    private static bool ValidateContinuationCallbacks(SemanticDocument document)
    {
        if (document.SchemaVersion < 10)
        {
            return document.ContinuationCallbacks.Count == 0;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        Dictionary<string, SemanticSymbol> symbolsById = document.Symbols.ToDictionary(
            symbol => symbol.Id,
            StringComparer.Ordinal);
        return document.ContinuationCallbacks.All(callback => callback is not null
                && callback.CallbackId > 0
                && (document.SchemaVersion < 12 || callback.CallbackId < CompilerCallbackIdStart)
                && !string.IsNullOrWhiteSpace(callback.Name)
                && !string.IsNullOrWhiteSpace(callback.MethodSymbolId)
                && IsValidCallbackSpan(callback.Span, callback.Name, document.Source.Length)
                && callablesById.TryGetValue(callback.MethodSymbolId, out SemanticCallable? callable)
                && callable.HasBody
                && callable.IsStatic
                && !callable.IsConstructor
                && callable.Import is null
                && string.Equals(callable.ReturnTypeId, "type:void", StringComparison.Ordinal)
                && ContinuationParametersMatch(document.SchemaVersion, callback, callable)
                && symbolsById.TryGetValue(callback.MethodSymbolId, out SemanticSymbol? symbol)
                && string.Equals(symbol.Kind, "method", StringComparison.Ordinal)
                && string.Equals(symbol.Name, callback.Name, StringComparison.Ordinal)
                && symbol.IsStatic
                && string.Equals(symbol.Accessibility, "public", StringComparison.Ordinal)
                && string.Equals(symbol.TypeId, "type:void", StringComparison.Ordinal)
                && string.Equals(
                    symbol.ContainingSymbolId,
                    "symbol:" + callable.ContainingTypeId,
                    StringComparison.Ordinal)
                && Contains(symbol.Span, callback.Span))
            && document.ContinuationCallbacks
                .Select(callback => callback.CallbackId)
                .SequenceEqual(document.ContinuationCallbacks
                    .Select(callback => callback.CallbackId)
                    .OrderBy(callbackId => callbackId))
            && document.ContinuationCallbacks
                .Select(callback => callback.CallbackId)
                .Distinct()
                .Count() == document.ContinuationCallbacks.Count
            && Unique(document.ContinuationCallbacks.Select(callback => callback.MethodSymbolId));
    }

    private static bool ValidateAsyncMethods(SemanticDocument document)
    {
        if (document.SchemaVersion < 12)
        {
            return document.AsyncMethods.Count == 0;
        }

        if (document.AsyncMethods.Any(method => method is null)
            || !Unique(document.AsyncMethods.Select(method => method.MethodSymbolId))
            || !Unique(document.AsyncMethods.Select(method => method.ExportName))
            || !document.AsyncMethods.Select(method => method.Span.Start)
                .SequenceEqual(document.AsyncMethods
                    .Select(method => method.Span.Start)
                    .OrderBy(start => start)))
        {
            return false;
        }

        Dictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        Dictionary<string, SemanticSymbol> symbolsById = document.Symbols.ToDictionary(
            symbol => symbol.Id,
            StringComparer.Ordinal);
        HashSet<string> graphMethodIds = document.ControlFlowGraphs
            .Select(graph => graph.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        int expectedCallbackId = CompilerCallbackIdStart;
        int moduleAwaitCount = 0;
        foreach (SemanticAsyncMethod method in document.AsyncMethods)
        {
            if (method is null
                || string.IsNullOrWhiteSpace(method.MethodSymbolId)
                || string.IsNullOrWhiteSpace(method.ExportName)
                || method.CompilerLocals is null
                || method.Lowering is not (
                    SemanticAsyncMethod.ReentrantZeroHeapCpsLowering or
                    SemanticAsyncMethod.ContinuationCfgLowering)
                || method.Segments is null
                || method.Segments.Count == 0
                || method.Segments.Any(segment => segment is null)
                || !IsValidSpan(method.Span, document.Source.Length)
                || graphMethodIds.Contains(method.MethodSymbolId)
                || !callablesById.TryGetValue(method.MethodSymbolId, out SemanticCallable? callable)
                || !callable.HasBody
                || !callable.IsStatic
                || callable.IsConstructor
                || callable.Import is not null
                || callable.Parameters.Count != 0
                || callable.ReturnTypeId != "type:void"
                || callable.Export?.Name != method.ExportName)
            {
                return false;
            }

            int methodAwaitCount = method.Segments.Count(segment => segment.AwaitSite is not null);
            if (methodAwaitCount > MaximumAsyncAwaitsPerMethod)
            {
                return false;
            }
            moduleAwaitCount += methodAwaitCount;

            if (method.Lowering == SemanticAsyncMethod.ContinuationCfgLowering)
            {
                if (!ValidateAsyncControlFlowMethod(
                    document,
                    method,
                    symbolsById,
                    ref expectedCallbackId))
                {
                    return false;
                }
                continue;
            }

            Dictionary<string, int> localDeclarationSegments = new(StringComparer.Ordinal);
            Dictionary<string, int> resultUseSegments = new(StringComparer.Ordinal);
            Dictionary<string, string> stateTypeIds = new(StringComparer.Ordinal);
            AsyncFlowValidationBudget flowBudget = new();
            for (int index = 0; index < method.Segments.Count; ++index)
            {
                SemanticAsyncSegment segment = method.Segments[index];
                bool requiresAwaitSite = index < method.Segments.Count - 1;
                if (segment is null
                    || segment.Ordinal != index
                    || segment.Statements is null
                    || segment.Statements.Any(statement => statement is null)
                    || !IsValidSpan(segment.Span, document.Source.Length)
                    || (segment.AwaitSite is not null) != requiresAwaitSite)
                {
                    return false;
                }

                foreach (SemanticAsyncStatement statement in segment.Statements)
                {
                    if (statement is null
                        || statement.Operation is null
                        || !ValidateOperation(statement.Operation)
                        || !AllOperationsSupported(statement.Operation)
                        || !ValidateAsyncStatement(
                            document.SchemaVersion,
                            document.SemanticVersion,
                            statement,
                            flowBudget))
                    {
                        return false;
                    }

                    if (statement.TargetSymbolId is { } targetSymbolId
                        && (!localDeclarationSegments.TryAdd(targetSymbolId, segment.Ordinal)
                            || resultUseSegments.ContainsKey(targetSymbolId)
                            || !IsMethodLocal(
                                symbolsById,
                                targetSymbolId,
                                method.MethodSymbolId,
                                statement.Operation.TypeId)))
                    {
                        return false;
                    }
                    if (statement.TargetSymbolId is { } stateSymbolId)
                    {
                        stateTypeIds.Add(stateSymbolId, statement.Operation.TypeId!);
                    }
                    foreach (SemanticOperation declaration in EnumerateOperations(statement.Operation)
                        .Where(operation => operation.Kind
                            == SemanticAsyncMethod.LocalDeclarationOperationKind))
                    {
                        if (declaration.SymbolId is not { } declarationSymbolId
                            || declaration.TypeId is not { } declarationTypeId
                            || !localDeclarationSegments.TryAdd(
                                declarationSymbolId,
                                segment.Ordinal)
                            || resultUseSegments.ContainsKey(declarationSymbolId)
                            || !IsMethodLocal(
                                symbolsById,
                                declarationSymbolId,
                                method.MethodSymbolId,
                                declarationTypeId))
                        {
                            return false;
                        }
                        stateTypeIds.Add(declarationSymbolId, declarationTypeId);
                    }
                }

                if (segment.AwaitSite is { } awaitSite)
                {
                    if (awaitSite.CallbackId != expectedCallbackId++
						|| !ValidateAsyncAwaitSite(
							awaitSite,
							document.Source.Length,
							method.MethodSymbolId,
							symbolsById,
							document,
							document.Callables))
                    {
                        return false;
                    }

                    if (awaitSite.ResultSymbolId is { } resultSymbolId
                        && (!resultUseSegments.TryAdd(resultSymbolId, segment.Ordinal + 1)
                            || localDeclarationSegments.ContainsKey(resultSymbolId)))
                    {
                        return false;
                    }
                    if (awaitSite.ResultSymbolId is { } stateResultSymbolId)
                    {
                        stateTypeIds.Add(stateResultSymbolId, awaitSite.ResultTypeId!);
                    }
                }
            }

            Dictionary<string, int> declarationSegments = localDeclarationSegments
                .Concat(resultUseSegments)
                .ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal);
            Dictionary<string, int> lastUseSegments = new(StringComparer.Ordinal);
            foreach (SemanticAsyncSegment segment in method.Segments)
            {
                IEnumerable<SemanticOperation> operations = segment.Statements
                    .Select(statement => statement.Operation)
                    .Concat(segment.AwaitSite?.Arguments ?? Array.Empty<SemanticOperation>());
                if (segment.AwaitSite?.CancellationToken is { } cancellationToken)
                {
                    operations = operations.Append(cancellationToken);
                }
                foreach (SemanticOperation operation in operations.SelectMany(EnumerateOperations))
                {
                    if (operation.SymbolId is not { } symbolId
                        || !declarationSegments.TryGetValue(symbolId, out int declarationSegment))
                    {
                        continue;
                    }
                    if (segment.Ordinal < declarationSegment
                        || document.SchemaVersion < 15 && segment.Ordinal != declarationSegment)
                    {
                        return false;
                    }
                    lastUseSegments[symbolId] = Math.Max(
                        lastUseSegments.GetValueOrDefault(symbolId),
                        segment.Ordinal);
                }
            }

            if (document.SchemaVersion < 15)
            {
                if (method.Segments.Any(segment => segment.AwaitSite?.StateFrame is not null))
                {
                    return false;
                }
                continue;
            }

            SemanticAsyncStateFlowAnalysis currentStateAnalysis =
                SemanticAsyncStateFlowAnalyzer.Analyze(method.Segments);
            if (UsesExactAsyncStateFlow(document.SemanticVersion)
                && currentStateAnalysis.Issues.Count > 0)
            {
                return false;
            }

            foreach (SemanticAsyncSegment segment in method.Segments.Where(item => item.AwaitSite is not null))
            {
                SemanticAsyncAwaitSite awaitSite = segment.AwaitSite!;
                IReadOnlyList<SemanticAsyncStateSlot> expectedSlots =
                    UsesExactAsyncStateFlow(document.SemanticVersion)
                        ? currentStateAnalysis.SlotsByAwaitSegment.GetValueOrDefault(segment.Ordinal)
                            ?? Array.Empty<SemanticAsyncStateSlot>()
                        : declarationSegments
                            .Where(pair => pair.Value <= segment.Ordinal
                                && lastUseSegments.TryGetValue(pair.Key, out int lastUse)
                                && lastUse > segment.Ordinal)
                            .OrderBy(pair => pair.Key, StringComparer.Ordinal)
                            .Select(pair => new SemanticAsyncStateSlot(
                                pair.Key,
                                stateTypeIds[pair.Key]))
                            .ToArray();
                SemanticAsyncStateFrame? frame = awaitSite.StateFrame;
                if (expectedSlots.Count == 0)
                {
                    if (frame is not null)
                    {
                        return false;
                    }
                    continue;
                }
                if (expectedSlots.Count > MaximumAsyncStateSlotsPerAwait
                    || frame is null
                    || frame.TypeId != $"type:synthetic:async_state:{awaitSite.CallbackId}"
                    || frame.Slots is null
                    || frame.Slots.Count != expectedSlots.Count
                    || !frame.Slots.Select(slot => (slot.SymbolId, slot.TypeId))
                        .SequenceEqual(expectedSlots.Select(slot => (slot.SymbolId, slot.TypeId)))
                    || !Unique(frame.Slots.Select(slot => slot.SymbolId)))
                {
                    return false;
                }
            }

            bool hasStateFrames = method.Segments.Any(segment => segment.AwaitSite?.StateFrame is not null);
            if (hasStateFrames
                && (!HasContinuationStateImport(document.Callables, "continuation_state_store")
                    || !HasContinuationStateImport(document.Callables, "continuation_state_read")))
            {
                return false;
            }
        }

        return moduleAwaitCount <= MaximumAsyncAwaitsPerModule;
    }

    private static bool ValidateAsyncControlFlowMethod(
        SemanticDocument document,
        SemanticAsyncMethod method,
        IReadOnlyDictionary<string, SemanticSymbol> symbolsById,
        ref int expectedCallbackId)
    {
        if (document.SemanticVersion != SemanticContract.CurrentSemanticVersion
            || method.EntrySegmentOrdinal < 0
            || method.EntrySegmentOrdinal >= method.Segments.Count
            || method.Segments.Count > SemanticAsyncMethod.MaximumControlFlowSegments
            || !ValidateCompilerLocals(document, method, symbolsById))
        {
            return false;
        }

        IReadOnlyDictionary<string, SemanticAsyncCompilerLocal> compilerLocalsById =
            method.CompilerLocals.ToDictionary(local => local.SymbolId, StringComparer.Ordinal);
        HashSet<int> validTargets = method.Segments
            .Select(segment => segment.Ordinal)
            .ToHashSet();
        Dictionary<string, string> localTypes = new(StringComparer.Ordinal);
        AsyncFlowValidationBudget flowBudget = new();
        int expectedSegmentOrdinal = 0;
        foreach (SemanticAsyncSegment segment in method.Segments)
        {
            if (segment.Ordinal < 0
                || segment.Ordinal >= method.Segments.Count
                || segment.Ordinal != expectedSegmentOrdinal++
                || segment.Statements is null
                || segment.Statements.Any(statement => statement is null)
                || segment.Transfer is null
                || !IsValidSpan(segment.Span, document.Source.Length))
            {
                return false;
            }

            foreach (SemanticAsyncStatement statement in segment.Statements)
            {
                if (statement.Operation is null
                    || !ValidateOperation(statement.Operation)
                    || !AllOperationsSupported(statement.Operation)
                    || !ValidateAsyncStatement(
                        document.SchemaVersion,
                        document.SemanticVersion,
                        statement,
                        flowBudget))
                {
                    return false;
                }

                if (statement.TargetSymbolId is { } targetSymbolId
                    && (!TryAddMethodLocal(
                            symbolsById,
                        method.MethodSymbolId,
                        targetSymbolId,
                        statement.Operation.TypeId,
                        compilerLocalsById,
                        localTypes)))
                {
                    return false;
                }
                foreach (SemanticOperation declaration in EnumerateOperations(statement.Operation)
                    .Where(operation => operation.Kind
                        == SemanticAsyncMethod.LocalDeclarationOperationKind))
                {
                    if (!TryAddMethodLocal(
                        symbolsById,
                        method.MethodSymbolId,
                        declaration.SymbolId,
                        declaration.TypeId,
                        compilerLocalsById,
                        localTypes))
                    {
                        return false;
                    }
                }
            }

            SemanticAsyncControlTransfer transfer = segment.Transfer;
            bool validTransfer = transfer.Kind switch
            {
                SemanticAsyncMethod.GotoTransferKind =>
                    segment.AwaitSite is null
                    && transfer.Condition is null
                    && validTargets.Contains(transfer.PrimaryTarget)
                    && transfer.SecondaryTarget == -1,
                SemanticAsyncMethod.BranchTransferKind =>
                    segment.AwaitSite is null
                    && transfer.Condition is not null
                    && transfer.Condition.TypeId == "type:bool"
                    && ValidateOperation(transfer.Condition)
                    && AllOperationsSupported(transfer.Condition)
                    && validTargets.Contains(transfer.PrimaryTarget)
                    && validTargets.Contains(transfer.SecondaryTarget),
                SemanticAsyncMethod.AwaitTransferKind =>
                    segment.AwaitSite is not null
                    && transfer.Condition is null
                    && validTargets.Contains(transfer.PrimaryTarget)
                    && transfer.SecondaryTarget == -1,
                SemanticAsyncMethod.ReturnTransferKind =>
                    segment.AwaitSite is null
                    && transfer.Condition is null
                    && transfer.PrimaryTarget == -1
                    && transfer.SecondaryTarget == -1,
                _ => false,
            };
            if (!validTransfer)
            {
                return false;
            }

            if (segment.AwaitSite is { } awaitSite)
            {
                if (awaitSite.CallbackId != expectedCallbackId++
                    || !ValidateAsyncAwaitSite(
                        awaitSite,
                        document.Source.Length,
                        method.MethodSymbolId,
                        symbolsById,
                        document,
                        document.Callables))
                {
                    return false;
                }
                if (awaitSite.ResultSymbolId is { } resultSymbolId
                    && !TryAddMethodLocal(
                        symbolsById,
                        method.MethodSymbolId,
                        resultSymbolId,
                        awaitSite.ResultTypeId,
                        compilerLocalsById,
                        localTypes))
                {
                    return false;
                }
            }
        }

        HashSet<int> reachable = new();
        Stack<int> pending = new();
        pending.Push(method.EntrySegmentOrdinal);
        while (pending.Count > 0)
        {
            int ordinal = pending.Pop();
            if (!reachable.Add(ordinal))
            {
                continue;
            }
            SemanticAsyncControlTransfer transfer = method.Segments[ordinal].Transfer!;
            if (transfer.PrimaryTarget >= 0)
            {
                pending.Push(transfer.PrimaryTarget);
            }
            if (transfer.SecondaryTarget >= 0)
            {
                pending.Push(transfer.SecondaryTarget);
            }
        }
        if (reachable.Count != method.Segments.Count)
        {
            return false;
        }

        SemanticAsyncStateFlowAnalysis stateAnalysis =
            SemanticAsyncStateFlowAnalyzer.AnalyzeControlFlow(method.Segments);
        if (stateAnalysis.Issues.Count > 0)
        {
            return false;
        }
        foreach (SemanticAsyncSegment segment in method.Segments
            .Where(item => item.AwaitSite is not null))
        {
            SemanticAsyncAwaitSite awaitSite = segment.AwaitSite!;
            IReadOnlyList<SemanticAsyncStateSlot> expectedSlots =
                stateAnalysis.SlotsByAwaitSegment.GetValueOrDefault(segment.Ordinal)
                ?? Array.Empty<SemanticAsyncStateSlot>();
            SemanticAsyncStateFrame? frame = awaitSite.StateFrame;
            if (expectedSlots.Count == 0)
            {
                if (frame is not null)
                {
                    return false;
                }
                continue;
            }
            if (expectedSlots.Count > MaximumAsyncStateSlotsPerAwait
                || frame is null
                || frame.TypeId != $"type:synthetic:async_state:{awaitSite.CallbackId}"
                || frame.Slots.Count != expectedSlots.Count
                || !frame.Slots.Select(slot => (slot.SymbolId, slot.TypeId))
                    .SequenceEqual(expectedSlots.Select(slot => (slot.SymbolId, slot.TypeId)))
                || !Unique(frame.Slots.Select(slot => slot.SymbolId)))
            {
                return false;
            }
        }

        bool hasStateFrames = method.Segments.Any(segment =>
            segment.AwaitSite?.StateFrame is not null);
        return !hasStateFrames
            || HasContinuationStateImport(document.Callables, "continuation_state_store")
                && HasContinuationStateImport(document.Callables, "continuation_state_read");
    }

    private static bool TryAddMethodLocal(
        IReadOnlyDictionary<string, SemanticSymbol> symbolsById,
        string methodSymbolId,
        string? symbolId,
        string? typeId,
        IReadOnlyDictionary<string, SemanticAsyncCompilerLocal> compilerLocalsById,
        IDictionary<string, string> localTypes)
    {
        return symbolId is not null
            && typeId is not null
            && (IsMethodLocal(symbolsById, symbolId, methodSymbolId, typeId)
                || compilerLocalsById.TryGetValue(symbolId, out SemanticAsyncCompilerLocal? local)
                    && local.TypeId == typeId)
            && localTypes.TryAdd(symbolId, typeId);
    }

    private static bool ValidateCompilerLocals(
        SemanticDocument document,
        SemanticAsyncMethod method,
        IReadOnlyDictionary<string, SemanticSymbol> symbolsById)
    {
        if (document.SchemaVersion < 16)
        {
            return method.CompilerLocals.Count == 0;
        }
        string prefix = $"symbol:compiler_local:{method.MethodSymbolId}:foreach:";
        return method.CompilerLocals.Count <= SemanticAsyncMethod.MaximumControlFlowSegments * 2
            && method.CompilerLocals.All(local => local is not null
                && local.SymbolId.StartsWith(prefix, StringComparison.Ordinal)
                && !string.IsNullOrWhiteSpace(local.Name)
                && document.Types.Any(type => type.Id == local.TypeId)
                && IsValidSpan(local.Span, document.Source.Length)
                && Contains(method.Span, local.Span)
                && !symbolsById.ContainsKey(local.SymbolId))
            && Unique(method.CompilerLocals.Select(local => local.SymbolId))
            && Unique(method.CompilerLocals.Select(local => local.Name))
            && method.CompilerLocals.Select(local => local.SymbolId)
                .SequenceEqual(method.CompilerLocals
                    .Select(local => local.SymbolId)
                    .OrderBy(id => id, StringComparer.Ordinal));
    }

    private static bool UsesExactAsyncStateFlow(string semanticVersion)
    {
        return semanticVersion is "1.16"
            || semanticVersion == SemanticContract.CurrentSemanticVersion;
    }

    private static bool ValidateAsyncAwaitSite(
        SemanticAsyncAwaitSite awaitSite,
        int sourceLength,
        string methodSymbolId,
        IReadOnlyDictionary<string, SemanticSymbol> symbolsById,
        SemanticDocument document,
        IReadOnlyList<SemanticCallable> callables)
    {
        if (awaitSite is null
            || awaitSite.CallbackId < CompilerCallbackIdStart
            || awaitSite.Arguments is null
            || awaitSite.Arguments.Any(argument => argument is null
                || !ValidateOperation(argument)
                || !AllOperationsSupported(argument))
            || awaitSite.CancellationToken is { } cancellationToken
                && (!ValidateOperation(cancellationToken)
                    || !AllOperationsSupported(cancellationToken)
                    || cancellationToken.TypeId
                        != "type:global::AvidScript.AvidCancellationToken"
                    || !CSharpLatentStoragePlanner.TryBuildSingleValue(
                        document,
                        cancellationToken,
                        "type:int64",
                        out _)
                    || callables.Count(callable =>
                        callable.Import is { Module: "env", Name: "continuation_bind_cancel" }
                        && callable.ReturnTypeId == "type:int32"
                        && callable.Parameters.Count == 2
                        && callable.Parameters.All(parameter =>
                            parameter.TypeId == "type:int64"
                            && parameter.RefKind == "none")) != 1)
            || !IsValidSpan(awaitSite.Span, sourceLength))
        {
            return false;
        }

        if (awaitSite.ProducerKind == "delay")
        {
            return awaitSite.PayloadKind == SemanticContinuationCallback.NonePayloadKind
                && awaitSite.Arguments.Count == 1
                && awaitSite.Arguments[0].TypeId == "type:float32"
                && awaitSite.ResultSymbolId is null
                && awaitSite.ResultTypeId is null;
        }

        if (awaitSite.ProducerKind == "next_tick")
        {
            return awaitSite.PayloadKind == SemanticContinuationCallback.NonePayloadKind
                && awaitSite.Arguments.Count == 0
                && awaitSite.ResultSymbolId is null
                && awaitSite.ResultTypeId is null;
        }

		if (awaitSite.ProducerKind.StartsWith(
			"binding_latent|",
			StringComparison.Ordinal))
		{
			string[] identity = awaitSite.ProducerKind.Split('|');
			if (identity.Length != 3
				|| string.IsNullOrWhiteSpace(identity[1])
				|| string.IsNullOrWhiteSpace(identity[2]))
			{
				return false;
			}

			SemanticCallable[] imports = callables.Where(callable =>
				callable.Import is { } import
				&& import.Module == identity[1]
				&& import.Name == identity[2]
				&& callable.ReturnTypeId == "type:int64"
				&& callable.Parameters.Count >= awaitSite.Arguments.Count + 1
				&& callable.Parameters[^1].TypeId == "type:int32"
				&& callable.Parameters[^1].RefKind == "none").ToArray();
			if (imports.Length != 1
				|| !CSharpLatentStoragePlanner.TryBuild(
					document,
					awaitSite.Arguments,
					imports[0].Parameters.Take(imports[0].Parameters.Count - 1).ToArray(),
					out _))
			{
				return false;
			}

			if (awaitSite.PayloadKind == SemanticContinuationCallback.NonePayloadKind)
			{
				return awaitSite.ResultSymbolId is null
					&& awaitSite.ResultTypeId is null
					&& awaitSite.BindingOrdinal == -1
					&& awaitSite.PayloadDescriptorTypeId is null
					&& awaitSite.PayloadValueTypeId is null;
			}
			if (document.SchemaVersion < 13
				|| awaitSite.PayloadKind
					!= SemanticContinuationCallback.ResultSlotPayloadKind
				|| awaitSite.BindingOrdinal < 0
				|| awaitSite.PayloadDescriptorTypeId is null
				|| !IsSha256(awaitSite.PayloadDescriptorTypeId)
				|| string.IsNullOrWhiteSpace(awaitSite.PayloadValueTypeId)
				|| string.IsNullOrWhiteSpace(awaitSite.ResultTypeId)
				|| !document.Types.Any(type => type.Id == awaitSite.PayloadValueTypeId)
				|| !document.TypeShapes.Any(shape =>
					shape.TypeId == awaitSite.ResultTypeId
					&& shape.GenericArgumentTypeId == awaitSite.PayloadValueTypeId)
				|| callables.Count(callable =>
					callable.Import is { Module: "env", Name: "continuation_result_read" }
					&& callable.ReturnTypeId == "type:int32"
					&& callable.Parameters.Count == 5
					&& callable.Parameters.All(parameter =>
						parameter.TypeId == "type:int32"
						&& parameter.RefKind == "none")) != 1)
			{
				return false;
			}
			return awaitSite.ResultSymbolId is null
				|| IsMethodLocal(
					symbolsById,
					awaitSite.ResultSymbolId,
					methodSymbolId,
					awaitSite.ResultTypeId);
		}

        if (awaitSite.ProducerKind != "object_load"
            || awaitSite.PayloadKind != SemanticContinuationCallback.ObjectPayloadKind
            || awaitSite.Arguments.Count != 1
            || !IsValidAssetPathConstant(awaitSite.Arguments[0]))
        {
            return false;
        }

        if (awaitSite.ResultSymbolId is null || awaitSite.ResultTypeId is null)
        {
            return awaitSite.ResultSymbolId is null && awaitSite.ResultTypeId is null;
        }

        return awaitSite.ResultTypeId == "type:global::AvidScript.AvidLoadedObject"
            && IsMethodLocal(
                symbolsById,
                awaitSite.ResultSymbolId,
                methodSymbolId,
                awaitSite.ResultTypeId);
    }

    private static bool IsMethodLocal(
        IReadOnlyDictionary<string, SemanticSymbol> symbolsById,
        string symbolId,
        string methodSymbolId,
        string? expectedTypeId)
    {
        return symbolsById.TryGetValue(symbolId, out SemanticSymbol? symbol)
            && symbol.Kind == "local"
            && symbol.ContainingSymbolId == methodSymbolId
            && !string.IsNullOrWhiteSpace(symbol.TypeId)
            && (expectedTypeId is null || symbol.TypeId == expectedTypeId);
    }

    private static bool IsValidAssetPathConstant(SemanticOperation operation)
    {
        string? assetPath = operation.TypeId == "type:string"
            && operation.Constant is { Kind: "string" } constant
            ? constant.Value
            : null;
        if (string.IsNullOrEmpty(assetPath)
            || assetPath[0] != '/'
            || assetPath.Length < 4
            || assetPath[1] == '/'
            || assetPath.IndexOf('\\') >= 0
            || assetPath.IndexOf('\0') >= 0
            || assetPath.Any(character => char.IsControl(character) || char.IsWhiteSpace(character))
            || assetPath.Contains("//", StringComparison.Ordinal))
        {
            return false;
        }

        int lastSlash = assetPath.LastIndexOf('/');
        int objectSeparator = assetPath.IndexOf('.', lastSlash + 1);
        return lastSlash > 0
            && objectSeparator > lastSlash + 1
            && objectSeparator < assetPath.Length - 1
            && assetPath.IndexOf('.', objectSeparator + 1) < 0
            && System.Text.Encoding.UTF8.GetByteCount(assetPath) <= 1024;
    }

    private static bool ContinuationParametersMatch(
        int schemaVersion,
        SemanticContinuationCallback callback,
        SemanticCallable callable)
    {
        if (schemaVersion == 10)
        {
            return callback.PayloadKind == SemanticContinuationCallback.NonePayloadKind
                && callable.Parameters.Count == 0;
        }

        return callback.PayloadKind switch
        {
            SemanticContinuationCallback.NonePayloadKind => callable.Parameters.Count == 0,
            SemanticContinuationCallback.ObjectPayloadKind => ParametersMatch(
                callable.Parameters,
                ObjectContinuationParameterTypeIds),
            _ => false,
        };
    }

    private static bool ParametersMatch(
        IReadOnlyList<SemanticCallableParameter> parameters,
        IReadOnlyList<string> expectedTypeIds)
    {
        return parameters.Count == expectedTypeIds.Count
            && parameters
                .OrderBy(parameter => parameter.Ordinal)
                .Select((parameter, ordinal) => parameter.Ordinal == ordinal
                    && parameter.RefKind == "none"
                    && string.Equals(parameter.TypeId, expectedTypeIds[ordinal], StringComparison.Ordinal))
                .All(matches => matches);
    }

    private static bool IsValidCallbackSpan(SemanticSpan span, string name, int sourceLength)
    {
        return span is not null
            && span.Start >= 0
            && span.Length == name.Length
            && span.End <= sourceLength
            && span.Line >= 0
            && span.Column >= 0
            && span.EndLine >= span.Line
            && span.EndColumn >= 0;
    }

    private static bool Contains(SemanticSpan owner, SemanticSpan child)
    {
        return owner is not null
            && child.Start >= owner.Start
            && child.End <= owner.End;
    }

    private static bool IsSupportedContract(int schemaVersion, string semanticVersion)
    {
        return (schemaVersion, semanticVersion) switch
        {
            (4, "1.4") => true,
            (5, "1.5") => true,
            (6, "1.6") => true,
            (7, "1.7") => true,
            (8, "1.8") => true,
            (9, "1.9") => true,
            (10, "1.10") => true,
            (11, "1.11") => true,
            (12, "1.12") => true,
            (13, "1.13") => true,
            (14, "1.14") => true,
            (15, "1.15") => true,
            (15, "1.16") => true,
            (15, "1.17") => true,
            (16, "1.18") => true,
            (SemanticContract.CurrentSchemaVersion, SemanticContract.CurrentSemanticVersion) => true,
            _ => false,
        };
    }

    private static bool HasContinuationStateImport(
        IReadOnlyList<SemanticCallable> callables,
        string name)
    {
        return callables.Count(callable =>
            callable.Import is { Module: "env", Name: var importName }
            && importName == name
            && callable.ReturnTypeId == "type:int32"
            && callable.Parameters.Count == 3
            && callable.Parameters[0].TypeId == "type:int64"
            && callable.Parameters[1].TypeId == "type:int32"
            && callable.Parameters[2].TypeId == "type:int32"
            && callable.Parameters.All(parameter => parameter.RefKind == "none")) == 1;
    }

    private static bool ValidateAsyncStatement(
        int schemaVersion,
        string semanticVersion,
        SemanticAsyncStatement statement,
        AsyncFlowValidationBudget budget)
    {
        if (statement.Operation.Kind == SemanticAsyncMethod.EarlyReturnGuardOperationKind)
        {
            return schemaVersion >= 14
                && statement.TargetSymbolId is null
                && statement.Operation.TypeId == "type:void"
                && statement.Operation.SymbolId is null
                && statement.Operation.OperatorKind is null
                && !statement.Operation.IsChecked
                && !statement.Operation.IsLifted
                && !statement.Operation.IsPostfix
                && !statement.Operation.IsTryCast
                && statement.Operation.TypeArgumentIds.Count == 0
                && statement.Operation.Constant is null
                && statement.Operation.Conversion is null
                && statement.Operation.InputConversion is null
                && statement.Operation.OutputConversion is null
                && statement.Operation.CaptureId is null
                && statement.Operation.Children.Count == 1
                && statement.Operation.Children[0].TypeId == "type:bool"
                && Contains(statement.Operation.Span, statement.Operation.Children[0].Span);
        }

        if (!IsStructuredAsyncFlow(statement.Operation))
        {
            return true;
        }
        return (semanticVersion is "1.16"
                || semanticVersion == SemanticContract.CurrentSemanticVersion)
            && statement.TargetSymbolId is null
            && ValidateStructuredAsyncFlow(
                statement.Operation,
                loopDepth: 0,
                depth: 0,
                budget);
    }

    private static bool ValidateStructuredAsyncFlow(
        SemanticOperation operation,
        int loopDepth,
        int depth,
        AsyncFlowValidationBudget budget)
    {
        if (depth > SemanticAsyncMethod.MaximumStructuredFlowDepth
            || ++budget.NodeCount > SemanticAsyncMethod.MaximumStructuredFlowNodes
            || !HasCanonicalFlowEnvelope(operation))
        {
            return false;
        }

        return operation.Kind switch
        {
            SemanticAsyncMethod.BlockOperationKind => operation.Children.All(child =>
                IsStructuredAsyncFlow(child)
                    ? ValidateStructuredAsyncFlow(
                        child,
                        loopDepth,
                        depth + 1,
                        budget)
                    : ValidateOperation(child)),
            SemanticAsyncMethod.LocalDeclarationOperationKind =>
                operation.SymbolId is not null
                && operation.TypeId is not null and not "type:void"
                && operation.Children.Count <= 1
                && operation.Children.All(ValidateOperation),
            SemanticAsyncMethod.IfOperationKind =>
                operation.Children.Count is 2 or 3
                && operation.Children[0].TypeId == "type:bool"
                && ValidateOperation(operation.Children[0])
                && operation.Children.Skip(1).All(child =>
                    child.Kind == SemanticAsyncMethod.BlockOperationKind
                    && ValidateStructuredAsyncFlow(
                        child,
                        loopDepth,
                        depth + 1,
                        budget)),
            SemanticAsyncMethod.WhileOperationKind =>
                operation.Children.Count == 2
                && operation.Children[0].TypeId == "type:bool"
                && ValidateOperation(operation.Children[0])
                && operation.Children[1].Kind == SemanticAsyncMethod.BlockOperationKind
                && ValidateStructuredAsyncFlow(
                    operation.Children[1],
                    loopDepth + 1,
                    depth + 1,
                    budget),
            SemanticAsyncMethod.DoWhileOperationKind =>
                operation.Children.Count == 2
                && operation.Children[0].Kind == SemanticAsyncMethod.BlockOperationKind
                && ValidateStructuredAsyncFlow(
                    operation.Children[0],
                    loopDepth + 1,
                    depth + 1,
                    budget)
                && operation.Children[1].TypeId == "type:bool"
                && ValidateOperation(operation.Children[1]),
            SemanticAsyncMethod.ForOperationKind =>
                operation.Children.Count == 4
                && operation.Children[0].Kind == SemanticAsyncMethod.BlockOperationKind
                && ValidateStructuredAsyncFlow(
                    operation.Children[0],
                    loopDepth,
                    depth + 1,
                    budget)
                && operation.Children[1].TypeId == "type:bool"
                && ValidateOperation(operation.Children[1])
                && operation.Children[2].Kind == SemanticAsyncMethod.BlockOperationKind
                && ValidateStructuredAsyncFlow(
                    operation.Children[2],
                    loopDepth,
                    depth + 1,
                    budget)
                && operation.Children[3].Kind == SemanticAsyncMethod.BlockOperationKind
                && ValidateStructuredAsyncFlow(
                    operation.Children[3],
                    loopDepth + 1,
                    depth + 1,
                    budget),
            SemanticAsyncMethod.BreakOperationKind or
            SemanticAsyncMethod.ContinueOperationKind =>
                loopDepth > 0 && operation.Children.Count == 0,
            SemanticAsyncMethod.ReturnOperationKind => operation.Children.Count == 0,
            _ => false,
        };
    }

    private static bool HasCanonicalFlowEnvelope(SemanticOperation operation)
    {
        bool localDeclaration = operation.Kind
            == SemanticAsyncMethod.LocalDeclarationOperationKind;
        return operation.TypeId is not null
            && (localDeclaration || operation.TypeId == "type:void")
            && (localDeclaration || operation.SymbolId is null)
            && operation.OperatorKind is null
            && !operation.IsChecked
            && !operation.IsLifted
            && !operation.IsPostfix
            && !operation.IsTryCast
            && operation.TypeArgumentIds.Count == 0
            && operation.Constant is null
            && operation.Conversion is null
            && operation.InputConversion is null
            && operation.OutputConversion is null
            && operation.CaptureId is null;
    }

    private static bool IsStructuredAsyncFlow(SemanticOperation operation)
    {
        return operation.Kind is
            SemanticAsyncMethod.BlockOperationKind or
            SemanticAsyncMethod.LocalDeclarationOperationKind or
            SemanticAsyncMethod.IfOperationKind or
            SemanticAsyncMethod.WhileOperationKind or
            SemanticAsyncMethod.DoWhileOperationKind or
            SemanticAsyncMethod.ForOperationKind or
            SemanticAsyncMethod.BreakOperationKind or
            SemanticAsyncMethod.ContinueOperationKind or
            SemanticAsyncMethod.ReturnOperationKind;
    }

    private sealed class AsyncFlowValidationBudget
    {
        public int NodeCount { get; set; }
    }

    private static bool IsOrdinalSorted(IReadOnlyList<string> values)
    {
        return values.SequenceEqual(values.OrderBy(value => value, StringComparer.Ordinal));
    }

    private static bool IsSha256(string value)
    {
        return value is not null
            && value.Length == 64
            && value.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f');
    }

    private static bool ValidateMethods(IReadOnlyList<SemanticMethodBody> methods)
    {
        return methods.All(method => method is not null
                && !string.IsNullOrWhiteSpace(method.MethodSymbolId)
                && method.Root is not null
                && ValidateOperation(method.Root))
            && Unique(methods.Select(method => method.MethodSymbolId));
    }

    private static bool ValidateGraphs(IReadOnlyList<SemanticControlFlowGraph> graphs)
    {
        if (graphs.Any(graph => graph is null)
            || !Unique(graphs.Select(graph => graph.MethodSymbolId)))
        {
            return false;
        }

        foreach (SemanticControlFlowGraph graph in graphs)
        {
            if (graph is null
                || string.IsNullOrWhiteSpace(graph.MethodSymbolId)
                || graph.Blocks is null
                || graph.Blocks.Any(block => block is null)
                || graph.EntryBlockOrdinal < 0
                || graph.ExitBlockOrdinal < 0
                || graph.Blocks.Select(block => block.Ordinal).Distinct().Count() != graph.Blocks.Count)
            {
                return false;
            }

            foreach (SemanticBasicBlock block in graph.Blocks)
            {
                if (block is null
                    || block.Ordinal < 0
                    || string.IsNullOrWhiteSpace(block.Kind)
                    || string.IsNullOrWhiteSpace(block.ConditionKind)
                    || block.Operations is null
                    || block.Predecessors is null
                    || block.Successors is null
                    || !block.Operations.All(ValidateOperation)
                    || (block.BranchValue is not null && !ValidateOperation(block.BranchValue))
                    || !block.Predecessors.All(ValidateEdge)
                    || !block.Successors.All(ValidateEdge))
                {
                    return false;
                }
            }
        }

        return true;
    }

    private static bool ValidateOperation(SemanticOperation operation)
    {
        return operation is not null
            && !string.IsNullOrWhiteSpace(operation.Kind)
            && operation.TypeArgumentIds is not null
            && operation.TypeArgumentIds.All(typeId => !string.IsNullOrWhiteSpace(typeId))
            && operation.Span is not null
            && operation.Children is not null
            && (operation.Constant is null || !string.IsNullOrWhiteSpace(operation.Constant.Kind))
            && (operation.Conversion is null || !string.IsNullOrWhiteSpace(operation.Conversion.Kind))
            && (operation.InputConversion is null || !string.IsNullOrWhiteSpace(operation.InputConversion.Kind))
            && (operation.OutputConversion is null || !string.IsNullOrWhiteSpace(operation.OutputConversion.Kind))
            && operation.Children.All(ValidateOperation);
    }

    private static bool AllOperationsSupported(SemanticOperation operation)
    {
        return operation.IsSupported && operation.Children.All(AllOperationsSupported);
    }

    private static IEnumerable<SemanticOperation> EnumerateOperations(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
        {
            foreach (SemanticOperation descendant in EnumerateOperations(child))
            {
                yield return descendant;
            }
        }
    }

    private static bool IsValidSpan(SemanticSpan span, int sourceLength)
    {
        return span is not null
            && span.Start >= 0
            && span.Length >= 0
            && span.End <= sourceLength
            && span.Line >= 0
            && span.Column >= 0
            && span.EndLine >= span.Line
            && span.EndColumn >= 0;
    }

    private static bool ValidateEdge(SemanticControlFlowEdge edge)
    {
        return edge is not null
            && edge.SourceBlockOrdinal >= 0
            && edge.DestinationBlockOrdinal >= 0
            && !string.IsNullOrWhiteSpace(edge.Kind)
            && !string.IsNullOrWhiteSpace(edge.Semantics);
    }

    private static bool Unique(IEnumerable<string> values)
    {
        HashSet<string> seen = new(StringComparer.Ordinal);
        foreach (string value in values)
        {
            if (string.IsNullOrWhiteSpace(value) || !seen.Add(value))
            {
                return false;
            }
        }

        return true;
    }
}

using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal static class CSharpSemanticInputValidator
{
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
            && ValidateGameplayEventCallbacks(document)
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
        return shapes.All(shape => shape is not null && !string.IsNullOrWhiteSpace(shape.TypeId))
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
            .ToHashSet(StringComparer.Ordinal);
        string[] expectedRootIds = document.Callables
            .Where(callable => callable.Export is not null)
            .Select(callable => callable.MethodSymbolId)
            .Concat(callbackIds)
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
                || (callable.Export is null && !callbackIds.Contains(id)))
            || reachableIds.Any(id => !callablesById.ContainsKey(id))
            || (reachability.Mode is "export_roots" or "entrypoint_roots"
                && !reachability.RootCallableIds.SequenceEqual(expectedRootIds))
            || (reachability.Mode == "all_callables_compatibility"
                && (reachability.RootCallableIds.Count != 0
                    || reachableIds.Count != callablesById.Count)))
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
            (SemanticContract.CurrentSchemaVersion, SemanticContract.CurrentSemanticVersion) => true,
            _ => false,
        };
    }

    private static bool IsOrdinalSorted(IReadOnlyList<string> values)
    {
        return values.SequenceEqual(values.OrderBy(value => value, StringComparer.Ordinal));
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

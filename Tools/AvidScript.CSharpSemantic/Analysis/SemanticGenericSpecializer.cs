using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

internal sealed record SemanticGenericProjection(
    IReadOnlyList<SemanticSymbol> Symbols,
    IReadOnlyList<SemanticCallable> Callables,
    IReadOnlyList<SemanticMethodBody> Methods,
    IReadOnlyList<SemanticControlFlowGraph> Graphs,
    IReadOnlyList<SemanticDiagnostic> Diagnostics);

// A closed method is a separate executable symbol. The source definition and its
// Roslyn operations remain in the artifact for diagnostics and source mapping.
internal static class SemanticGenericSpecializer
{
    private const int MaximumInstances = 128;
    private const int MaximumDepth = 16;

    public static SemanticGenericProjection Project(
        IReadOnlyList<SemanticType> types,
        IReadOnlyList<SemanticTypeShape> typeShapes,
        IReadOnlyList<SemanticSymbol> sourceSymbols,
        IReadOnlyList<SemanticCallable> sourceCallables,
        IReadOnlyList<SemanticMethodBody> sourceMethods,
        IReadOnlyList<SemanticControlFlowGraph> sourceGraphs,
        IReadOnlySet<string> reachableSourceIds)
    {
        Dictionary<string, SemanticCallable> definitions = sourceCallables
            .Where(callable => callable.GenericTypeParameterIds?.Count > 0)
            .ToDictionary(callable => callable.MethodSymbolId, StringComparer.Ordinal);
        if (definitions.Count == 0)
            return new(sourceSymbols, sourceCallables, sourceMethods, sourceGraphs,
                Array.Empty<SemanticDiagnostic>());

        Dictionary<string, SemanticType> typesById = types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        Dictionary<string, SemanticTypeShape> shapesById = typeShapes.ToDictionary(
            shape => shape.TypeId, StringComparer.Ordinal);
        Dictionary<string, SemanticControlFlowGraph> graphsById = sourceGraphs.ToDictionary(
            graph => graph.MethodSymbolId, StringComparer.Ordinal);
        Dictionary<string, SemanticMethodBody> methodsById = sourceMethods.ToDictionary(
            method => method.MethodSymbolId, StringComparer.Ordinal);
        List<SemanticSymbol> symbols = sourceSymbols.ToList();
        List<SemanticCallable> callables = sourceCallables.ToList();
        List<SemanticMethodBody> methods = sourceMethods.ToList();
        List<SemanticControlFlowGraph> graphs = new();
        List<SemanticDiagnostic> diagnostics = new();
        Dictionary<string, int> instanceDepths = new(StringComparer.Ordinal);
        Queue<(string Id, int Depth)> pending = new();

        foreach (SemanticControlFlowGraph graph in sourceGraphs)
        {
            if (definitions.ContainsKey(graph.MethodSymbolId)
                || !reachableSourceIds.Contains(graph.MethodSymbolId))
            {
                graphs.Add(graph);
                continue;
            }
            graphs.Add(RewriteGraph(graph, new Dictionary<string, string>(StringComparer.Ordinal),
                new Dictionary<string, string>(StringComparer.Ordinal), 0));
        }

        while (pending.Count > 0)
        {
            (string instanceId, int depth) = pending.Dequeue();
            SemanticCallable instance = callables.Single(callable => callable.MethodSymbolId == instanceId);
            SemanticCallable definition = definitions[instance.GenericDefinitionSymbolId!];
            string definitionId = definition.MethodSymbolId;
            Dictionary<string, string> typeMap = definition.GenericTypeParameterIds!
                .Zip(instance.GenericArgumentTypeIds!)
                .ToDictionary(pair => pair.First, pair => pair.Second, StringComparer.Ordinal);
            Dictionary<string, string> symbolMap = sourceSymbols
                .Where(symbol => symbol.Id == definitionId
                    || symbol.ContainingSymbolId == definitionId)
                .ToDictionary(symbol => symbol.Id,
                    symbol => symbol.Id.Replace(definitionId, instanceId, StringComparison.Ordinal),
                    StringComparer.Ordinal);
            symbolMap[definitionId] = instanceId;

            foreach (SemanticSymbol symbol in sourceSymbols.Where(symbol => symbolMap.ContainsKey(symbol.Id)))
            {
                symbols.Add(symbol with
                {
                    Id = symbolMap[symbol.Id],
                    ContainingSymbolId = symbol.ContainingSymbolId == definitionId
                        ? instanceId : symbol.ContainingSymbolId,
                    TypeId = MapType(symbol.TypeId, typeMap, symbol.Span),
                });
            }
            if (graphsById.TryGetValue(definitionId, out SemanticControlFlowGraph? graph))
            {
                graphs.Add(RewriteGraph(graph with { MethodSymbolId = instanceId },
                    typeMap, symbolMap, depth));
            }
            if (methodsById.TryGetValue(definitionId, out SemanticMethodBody? method))
            {
                methods.Add(new SemanticMethodBody(instanceId,
                    RewriteOperation(method.Root, typeMap, symbolMap, depth)));
            }
        }

        return new(
            symbols.OrderBy(symbol => symbol.Id, StringComparer.Ordinal).ToArray(),
            callables.OrderBy(callable => callable.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            methods.OrderBy(method => method.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            graphs.OrderBy(graph => graph.MethodSymbolId, StringComparer.Ordinal).ToArray(),
            diagnostics.ToArray());

        string? MapType(string? typeId, IReadOnlyDictionary<string, string> typeMap, SemanticSpan span)
        {
            if (typeId is null) return null;
            if (!SemanticGenericTypeSubstitution.TryClose(
                    typeId, typeMap, typesById, shapesById, out string closed))
                diagnostics.Add(new SemanticDiagnostic("ASCS1064", "error",
                    $"Generic type '{typeId}' needs a structured closed type layout.", span));
            return closed;
        }

        SemanticControlFlowGraph RewriteGraph(
            SemanticControlFlowGraph graph,
            IReadOnlyDictionary<string, string> typeMap,
            IReadOnlyDictionary<string, string> symbolMap,
            int depth) => graph with
        {
            Blocks = graph.Blocks.Select(block => block with
            {
                Operations = block.Operations.Select(operation =>
                    RewriteOperation(operation, typeMap, symbolMap, depth)).ToArray(),
                BranchValue = block.BranchValue is null ? null
                    : RewriteOperation(block.BranchValue, typeMap, symbolMap, depth),
            }).ToArray(),
        };

        SemanticOperation RewriteOperation(
            SemanticOperation operation,
            IReadOnlyDictionary<string, string> typeMap,
            IReadOnlyDictionary<string, string> symbolMap,
            int depth)
        {
            string? symbolId = operation.SymbolId;
            string[] typeArguments = operation.TypeArgumentIds
                .Select(typeId => MapType(typeId, typeMap, operation.Span)!).ToArray();
            if (symbolId is not null && definitions.TryGetValue(symbolId, out SemanticCallable? target))
            {
                string[] formals = target.GenericTypeParameterIds!.ToArray();
                if (!target.IsStatic || target.IsConstructor || target.Import is not null
                    || target.Dispatch?.IsVirtual == true
                    || target.Dispatch?.IsAbstract == true
                    || formals.Length != typeArguments.Length
                    || typeArguments.Any(typeId => !typesById.TryGetValue(typeId, out SemanticType? type)
                        || type.Kind == "type_parameter"))
                {
                    diagnostics.Add(new SemanticDiagnostic("ASCS1064", "error",
                        $"Generic call '{symbolId}' has no supported closed static method layout.",
                        operation.Span));
                }
                else
                {
                    string instanceId = SemanticContract.GenericInstanceId(symbolId, typeArguments);
                    if (!instanceDepths.ContainsKey(instanceId))
                    {
                        if (instanceDepths.Count >= MaximumInstances || depth >= MaximumDepth)
                            diagnostics.Add(new SemanticDiagnostic("ASCS1064", "error",
                                "Generic specialization exceeded its instance or expansion budget.",
                                operation.Span));
                        else
                        {
                            instanceDepths.Add(instanceId, depth + 1);
                            Dictionary<string, string> targetTypes = formals.Zip(typeArguments)
                                .ToDictionary(pair => pair.First, pair => pair.Second, StringComparer.Ordinal);
                            callables.Add(target with
                            {
                                MethodSymbolId = instanceId,
                                ReturnTypeId = MapType(target.ReturnTypeId, targetTypes, operation.Span)!,
                                Parameters = target.Parameters.Select(parameter => parameter with
                                {
                                    SymbolId = parameter.SymbolId.Replace(
                                        symbolId, instanceId, StringComparison.Ordinal),
                                    TypeId = MapType(parameter.TypeId, targetTypes, operation.Span)!,
                                }).ToArray(),
                                GenericTypeParameterIds = Array.Empty<string>(),
                                GenericDefinitionSymbolId = symbolId,
                                GenericArgumentTypeIds = typeArguments,
                            });
                            pending.Enqueue((instanceId, depth + 1));
                        }
                    }
                    symbolId = instanceId;
                }
            }
            else if (symbolId is not null && symbolMap.TryGetValue(symbolId, out string? mapped))
                symbolId = mapped;

            return operation with
            {
                TypeId = MapType(operation.TypeId, typeMap, operation.Span),
                SymbolId = symbolId,
                TypeArgumentIds = typeArguments,
                Children = operation.Children.Select(child =>
                    RewriteOperation(child, typeMap, symbolMap, depth)).ToArray(),
            };
        }
    }
}

using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// This recognizes an ordinary-method normalization, not a new opcode or schema.
// Older artifacts with only HasInstanceInitializers remain ineligible: allocating
// them as zero-filled objects would silently discard observable source behavior.
public static class SemanticInstanceInitializerContract
{
    public static string MethodId(string typeId) => "symbol:compiler_method:" + typeId + ":instance_initializers";

    public static bool IsNormalized(SemanticDocument document, string definitionTypeId)
    {
        string id = MethodId(definitionTypeId);
        SemanticCallable? helper = document.Callables.SingleOrDefault(item => item.MethodSymbolId == id);
        SemanticMethodBody? body = document.Methods.SingleOrDefault(item => item.MethodSymbolId == id);
        SemanticControlFlowGraph? graph = document.ControlFlowGraphs.SingleOrDefault(item => item.MethodSymbolId == id);
        if (helper is null || helper.IsStatic || helper.IsConstructor || !helper.HasBody
            || helper.ContainingTypeId != definitionTypeId || helper.ReturnTypeId != "type:void"
            || helper.Parameters.Count != 0 || helper.Import is not null || helper.Export is not null
            || body?.Root.Kind != "block" || body.Root.Children.Count == 0 || graph is null)
            return false;
        string? owner = document.Symbols.SingleOrDefault(item => item.Kind == "type"
            && item.TypeId == definitionTypeId)?.Id;
        var fields = document.Symbols.Where(item => item.Kind == "field" && !item.IsStatic
            && item.ContainingSymbolId == owner).ToDictionary(item => item.Id, StringComparer.Ordinal);
        List<SemanticOperation> assignments = new();
        foreach (SemanticOperation statement in body.Root.Children)
        {
            if (statement.Kind != "expression_statement" || statement.Children.Count != 1
                || statement.Children[0] is not { Kind: "assignment", Children.Count: 2 } assignment
                || assignment.Children[0] is not { Kind: "field_reference", SymbolId: { } fieldId,
                    Children.Count: 1 } target
                || !fields.TryGetValue(fieldId, out SemanticSymbol? field)
                || assignment.Span != field.Span || target.TypeId != field.TypeId
                || target.Children[0].Kind != "instance_reference"
                || target.Children[0].TypeId != definitionTypeId)
                return false;
            assignments.Add(assignment);
        }
        string[] expected = assignments.Select(item => item.Children[0].SymbolId!).ToArray();
        if (expected.Distinct(StringComparer.Ordinal).Count() != expected.Length) return false;
        SemanticOperation[] stores = graph.Blocks.SelectMany(block => block.Operations)
            .SelectMany(Operations).Where(item => item.Kind == "assignment" && item.Children.Count == 2
                && item.Children[0].Kind == "field_reference"
                && expected.Contains(item.Children[0].SymbolId, StringComparer.Ordinal)).ToArray();
        if (!stores.Select(item => item.Children[0].SymbolId).SequenceEqual(expected)
            || stores.Where((store, index) => store.Span.Start < assignments[index].Span.Start
                || store.Span.End > assignments[index].Span.End).Any()) return false;

        var constructors = document.Callables.Where(item => item.IsConstructor && !item.IsStatic
            && item.ContainingTypeId == definitionTypeId && item.GenericDefinitionSymbolId is null)
            .ToDictionary(item => item.MethodSymbolId, StringComparer.Ordinal);
        if (constructors.Count == 0) return false;
        foreach (SemanticCallable constructor in constructors.Values)
        {
            var seen = new HashSet<string>(StringComparer.Ordinal);
            SemanticCallable current = constructor;
            while (true)
            {
                if (!seen.Add(current.MethodSymbolId) || !current.HasBody) return false;
                SemanticMethodBody? ctor = document.Methods.SingleOrDefault(item => item.MethodSymbolId == current.MethodSymbolId);
                SemanticControlFlowGraph? ctorGraph = document.ControlFlowGraphs.SingleOrDefault(item => item.MethodSymbolId == current.MethodSymbolId);
                SemanticOperation? first = ctor?.Root.Children.FirstOrDefault();
                if (first is not { Kind: "expression_statement", Children.Count: 1 }
                    || first.Children[0] is not { Kind: "invocation", SymbolId: { } target }) return false;
                if (target == id)
                {
                    if (!StartsWithHelper(ctorGraph, id) || CountCalls(ctorGraph, id) != 1) return false;
                    break;
                }
                if (CountCalls(ctorGraph, id) != 0 || CountCalls(ctorGraph, target) != 1
                    || !constructors.TryGetValue(target, out SemanticCallable? next)) return false;
                current = next;
            }
        }
        return true;
    }

    private static int CountCalls(SemanticControlFlowGraph? graph, string id) => graph?.Blocks
        .SelectMany(block => block.Operations.Concat(block.BranchValue is null
            ? Array.Empty<SemanticOperation>() : new[] { block.BranchValue }))
        .SelectMany(Operations).Count(operation => operation.Kind == "invocation" && operation.SymbolId == id) ?? 0;

    private static bool StartsWithHelper(SemanticControlFlowGraph? graph, string id)
    {
        if (graph is null) return false;
        int ordinal = graph.EntryBlockOrdinal;
        var visited = new HashSet<int>();
        while (visited.Add(ordinal))
        {
            SemanticBasicBlock? block = graph.Blocks.SingleOrDefault(item => item.Ordinal == ordinal);
            if (block is null || block.BranchValue is not null) return false;
            if (block.Operations.Count > 0)
                return block.Operations[0] is { Kind: "expression_statement", Children.Count: 1 } statement
                    && statement.Children[0] is { Kind: "invocation", Children.Count: 1 } call
                    && call.SymbolId == id && call.Children[0].Kind == "instance_reference";
            if (block.Successors.Count != 1) return false;
            ordinal = block.Successors[0].DestinationBlockOrdinal;
        }
        return false;
    }

    private static IEnumerable<SemanticOperation> Operations(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation nested in Operations(child)) yield return nested;
    }
}

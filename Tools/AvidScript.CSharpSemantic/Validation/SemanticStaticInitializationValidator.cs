using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticStaticInitializationValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document is null || document.SchemaVersion != SemanticStaticInitialization.SchemaVersion
            || document.SemanticVersion != SemanticStaticInitialization.SemanticVersion
            || document.StaticInitialization is not { Types: { Count: > 0 } } plan
            || !BaseVersion(plan) || document.Source is null || document.Types is null || document.Symbols is null || document.ClassTypes is null
            || document.Callables is null || document.Methods is null || document.ControlFlowGraphs is null)
            return false;
        if (document.Language != "csharp" || string.IsNullOrWhiteSpace(document.Source.SourceId)
            || !Hash(document.Source.Sha256) || document.Source.Length < 0
            || document.Types.Any(item => item is null || string.IsNullOrWhiteSpace(item.Id))
            || document.Symbols.Any(item => item is null || string.IsNullOrWhiteSpace(item.Id))
            || document.Callables.Any(item => item is null) || document.Methods.Any(item => item is null)
            || document.ControlFlowGraphs.Any(item => item is null) || document.ClassTypes.Any(item => item is null)
            || document.Symbols.GroupBy(item => item.Id).Any(group => group.Count() != 1)) return false;
        var symbols = document.Symbols.ToDictionary(item => item.Id, StringComparer.Ordinal);
        HashSet<string> owners = new(StringComparer.Ordinal);
        foreach (SemanticStaticTypeInitialization type in plan.Types)
        {
            if (type is null || string.IsNullOrWhiteSpace(type.TypeId) || !owners.Add(type.TypeId)
                || type.Fields is null || type.BeforeFieldInit != (type.ConstructorMethodId is null)
                || !document.Types.Any(item => item.Id == type.TypeId)) return false;
            var owner = document.Symbols.Where(item => item.Kind == "type" && item.TypeId == type.TypeId).ToArray();
            if (owner.Length != 1) return false;
            var expected = document.Symbols.Where(item => item.Kind == "field" && item.IsStatic
                && !item.IsConst && item.ContainingSymbolId == owner[0].Id).Select(item => item.Id).ToHashSet(StringComparer.Ordinal);
            HashSet<string> fields = new(StringComparer.Ordinal), sources = new(StringComparer.Ordinal);
            string? source = null, hash = null;
            int end = -1, length = -1;
            foreach (SemanticStaticFieldInitialization field in type.Fields)
            {
                if (field is null || string.IsNullOrWhiteSpace(field.FieldSymbolId) || !fields.Add(field.FieldSymbolId)
                    || !expected.Contains(field.FieldSymbolId) || !symbols.TryGetValue(field.FieldSymbolId, out var symbol)
                    || string.IsNullOrWhiteSpace(field.SourceId) || !Hash(field.SourceSha256)
                    || field.Span is null || field.Span != symbol.Span || field.SourceLength < 0
                    || field.Span.Start < 0 || field.Span.Length < 0
                    || field.Span.Start > field.SourceLength - field.Span.Length) return false;
                if (source != field.SourceId)
                {
                    if (!sources.Add(field.SourceId)) return false;
                    source = field.SourceId; hash = field.SourceSha256; length = field.SourceLength; end = -1;
                }
                if (field.Span.Start < end || field.SourceSha256 != hash || field.SourceLength != length) return false;
                end = field.Span.End;
                if (field.SourceId == document.Source.SourceId
                    && (field.SourceSha256 != document.Source.Sha256 || field.SourceLength != document.Source.Length)) return false;
                if (field.Initializer is null)
                {
                    if (field.ControlFlowGraph is not null) return false;
                    continue;
                }
                if (!Tree(field.Initializer, field.SourceLength)
                    || field.Initializer is not { Kind: "expression_statement", Children.Count: 1 } statement
                    || statement.Children[0] is not { Kind: "assignment", Children.Count: 2 } assignment
                    || !Target(assignment.Children[0], field, symbol) || statement.Span != field.Span
                    || field.ControlFlowGraph is not { Blocks: { Count: > 0 } } graph
                    || graph.MethodSymbolId != SemanticStaticInitialization.InitializerId(field.FieldSymbolId)
                    || graph.Blocks.Any(block => block is null || block.Operations is null
                        || block.Predecessors is null || block.Successors is null)
                    || graph.Blocks.Any(block => block.Operations.Any(operation => !Tree(operation, field.SourceLength))
                        || block.BranchValue is not null && !Tree(block.BranchValue, field.SourceLength))
                    || graph.Blocks.Select(block => block.Ordinal).Distinct().Count() != graph.Blocks.Count
                    || !graph.Blocks.Any(block => block.Ordinal == graph.EntryBlockOrdinal && block.Kind == "entry")
                    || !graph.Blocks.Any(block => block.Ordinal == graph.ExitBlockOrdinal && block.Kind == "exit")) return false;
                var blocks = graph.Blocks.ToDictionary(block => block.Ordinal);
                foreach (var block in graph.Blocks)
                {
                    foreach (var edge in block.Successors)
                        if (edge is null || edge.SourceBlockOrdinal != block.Ordinal
                            || !blocks.TryGetValue(edge.DestinationBlockOrdinal, out var destination)
                            || !destination.Predecessors.Contains(edge)) return false;
                    foreach (var edge in block.Predecessors)
                        if (edge is null || edge.DestinationBlockOrdinal != block.Ordinal
                            || !blocks.TryGetValue(edge.SourceBlockOrdinal, out var predecessor)
                            || !predecessor.Successors.Contains(edge)) return false;
                }
                var stores = graph.Blocks.SelectMany(block => block.Operations).SelectMany(Operations)
                    .Where(operation => operation.Kind == "assignment" && operation.Children.Count == 2
                        && operation.Children[0].SymbolId == field.FieldSymbolId).ToArray();
                if (stores.Length != 1 || !Target(stores[0].Children[0], field, symbol)) return false;
            }
            if (!expected.SetEquals(fields)) return false;
            if (type.ConstructorMethodId is { } id)
            {
                var constructors = document.Callables.Where(item => item.MethodSymbolId == id).ToArray();
                if (constructors.Length != 1 || !constructors[0].IsStatic || !constructors[0].HasBody
                    || !symbols.TryGetValue(id, out var constructorSymbol) || constructorSymbol.Name != ".cctor"
                    || constructors[0].ContainingTypeId != type.TypeId || constructors[0].ReturnTypeId != "type:void"
                    || constructors[0].Parameters is not { Count: 0 } || constructors[0].Import is not null || constructors[0].Export is not null
                    || !document.Methods.Any(item => item.MethodSymbolId == id)
                    || !document.ControlFlowGraphs.Any(item => item.MethodSymbolId == id)
                        && document.ExceptionFlows?.Any(item => item.MethodSymbolId == id) != true) return false;
            }
            else if (type.Fields.Count == 0) return false;
        }
        if (document.ClassTypes.Any(type => type.IsSourceDeclared && type.HasStaticInitialization
            && document.Symbols.Any(symbol => symbol.Kind == "type" && symbol.TypeId == type.TypeId)
            && !owners.Contains(type.TypeId))) return false;
        // Default-initialized fields also own storage, including each closed
        // generic type even when Roslyn does not generate a .cctor.
        if (document.Symbols.Any(field => field.Kind == "field" && field.IsStatic && !field.IsConst
            && field.ContainingSymbolId is { } id && symbols.TryGetValue(id, out var owner)
            && owner.Kind == "type" && owner.TypeId is { } typeId && !owners.Contains(typeId))) return false;
        return SemanticStaticFieldAccessValidator.IsValid(document, requireOwners: true);
    }

    private static bool BaseVersion(SemanticStaticInitializationPlan plan) =>
        plan.BaseSchemaVersion is >= 31 and <= 47
        && (SemanticContract.IsCurrentOrPrevious(plan.BaseSchemaVersion, plan.BaseSemanticVersion)
            || plan.BaseSchemaVersion == SemanticContract.ExceptionFlowSchemaVersion
                && plan.BaseSemanticVersion == SemanticContract.ExceptionFlowSemanticVersion);

    private static bool Target(SemanticOperation target, SemanticStaticFieldInitialization field, SemanticSymbol symbol) =>
        target is { Kind: "field_reference", Children.Count: 0 }
        && target.SymbolId == field.FieldSymbolId && target.TypeId == symbol.TypeId;

    private static bool Hash(string value) => value is { Length: 64 }
        && value.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static bool Tree(SemanticOperation? operation, int sourceLength, int depth = 0) =>
        depth < SemanticSerializer.MaximumDepth && operation is { Children: not null, Span: not null }
        && !string.IsNullOrWhiteSpace(operation.Kind) && operation.Span.Start >= 0 && operation.Span.Length >= 0
        && operation.Span.Start <= sourceLength - operation.Span.Length
        && operation.Children.All(child => Tree(child, sourceLength, depth + 1));

    private static IEnumerable<SemanticOperation> Operations(SemanticOperation operation)
    {
        yield return operation;
        foreach (SemanticOperation child in operation.Children)
            foreach (SemanticOperation nested in Operations(child)) yield return nested;
    }
}

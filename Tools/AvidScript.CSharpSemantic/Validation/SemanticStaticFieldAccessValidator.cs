using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Covers every executable/source operation surface, including operations that
// have moved out of a method CFG into async or exception-flow records.
public static class SemanticStaticFieldAccessValidator
{
    public static bool IsValid(SemanticDocument document, bool requireOwners)
    {
        if (document is null || document.Methods is null || document.ControlFlowGraphs is null
            || document.AsyncMethods is null || document.Symbols is null || document.Types is null
            || document.TypeShapes is null) return false;
        if (requireOwners && (document.StaticInitialization?.Types is null
            || document.Types.Any(item => item is null || string.IsNullOrWhiteSpace(item.Id))
            || document.Symbols.Any(item => item is null || string.IsNullOrWhiteSpace(item.Id))
            || document.TypeShapes.Any(item => item is null || string.IsNullOrWhiteSpace(item.TypeId))
            || document.Types.Select(item => item.Id).Distinct().Count() != document.Types.Count
            || document.Symbols.Select(item => item.Id).Distinct().Count() != document.Symbols.Count
            || document.TypeShapes.Select(item => item.TypeId).Distinct().Count() != document.TypeShapes.Count)) return false;
        var symbols = requireOwners ? document.Symbols.ToDictionary(item => item.Id, StringComparer.Ordinal) : new();
        var types = requireOwners ? document.Types.ToDictionary(item => item.Id, StringComparer.Ordinal) : new();
        var shapes = requireOwners ? document.TypeShapes.ToDictionary(item => item.TypeId, StringComparer.Ordinal) : new();
        foreach (var method in document.Methods)
            if (method is null || !Tree(method.Root)) return false;
        foreach (var graph in document.ControlFlowGraphs)
            if (!Graph(graph)) return false;
        foreach (var method in document.AsyncMethods)
            if (method is null || !Segments(method.Segments)) return false;
        foreach (var flow in (document.ExceptionFlows ?? Array.Empty<SemanticExceptionFlow>())
            .Concat(document.RejectedAsyncExceptionFlows ?? Array.Empty<SemanticExceptionFlow>()))
        {
            if (flow is null) return false;
            if (flow.Blocks is { } blocks)
                foreach (var block in blocks)
                    if (block is null || !Roots(block.Operations, block.BranchValue)) return false;
            if (flow.AsyncContinuationPreview is { } preview && !Segments(preview.Segments)) return false;
        }
        if (document.StaticInitialization is { } initialization)
        {
            if (initialization.Types is null) return false;
            foreach (var type in initialization.Types)
            {
                if (type?.Fields is null) return false;
                foreach (var field in type.Fields)
                    if (field is null || field.Initializer is { } initializer && !Tree(initializer)
                        || field.ControlFlowGraph is { } graph && !Graph(graph)) return false;
            }
        }
        return true;

        bool Graph(SemanticControlFlowGraph? graph) => graph?.Blocks is { } blocks
            && blocks.All(block => block is not null && Roots(block.Operations, block.BranchValue));

        bool Roots(IReadOnlyList<SemanticOperation>? operations, SemanticOperation? branch) =>
            operations is not null && operations.All(operation => Tree(operation)) && (branch is null || Tree(branch));

        bool Segments(IReadOnlyList<SemanticAsyncSegment>? segments)
        {
            if (segments is null) return false;
            foreach (var segment in segments)
            {
                if (segment?.Statements is null
                    || segment.Statements.Any(statement => statement is null || !Tree(statement.Operation))
                    || segment.Transfer?.Condition is { } condition && !Tree(condition)) return false;
                if (segment.AwaitSite is { } site && (!Roots(site.Arguments, site.CancellationToken)
                    || site.MemberAssignment is { } member && !Tree(member.Target))) return false;
            }
            return true;
        }

        bool Tree(SemanticOperation? operation, int depth = 0)
        {
            if (operation?.Children is null || depth >= SemanticSerializer.MaximumDepth || !Access(operation)) return false;
            return operation.Children.All(child => Tree(child, depth + 1));
        }

        bool Access(SemanticOperation operation)
        {
            if (!requireOwners) return operation.StaticFieldOwnerTypeId is null;
            SemanticSymbol? field = operation.SymbolId is { } id ? symbols.GetValueOrDefault(id) : null;
            bool staticField = operation.Kind == "field_reference" && field is { Kind: "field", IsStatic: true, IsConst: false };
            if (!staticField) return operation.StaticFieldOwnerTypeId is null;
            if (operation.StaticFieldOwnerTypeId is not { } owner || !types.ContainsKey(owner)
                || operation.Children.Count != 0 || operation.TypeArgumentIds is not { Count: 0 }
                || field!.ContainingSymbolId is not { } container
                || !symbols.TryGetValue(container, out var declaration)
                || declaration.Kind != "type" || declaration.TypeId is not { } definition
                || !document.StaticInitialization!.Types.Any(plan => plan?.TypeId == definition)) return false;
            if (owner == definition) return operation.TypeId == field.TypeId;
            if (!shapes.TryGetValue(owner, out var instance) || instance.GenericDefinitionTypeId != definition
                || instance.GenericArgumentTypeIds is not { } arguments
                || !shapes.TryGetValue(definition, out var generic) || generic.GenericArgumentTypeIds is not { } parameters
                || arguments.Count != parameters.Count || parameters.Distinct().Count() != parameters.Count
                || arguments.Any(argument => argument is null || !types.ContainsKey(argument))
                || parameters.Any(parameter => parameter is null || !types.TryGetValue(parameter, out var type)
                    || type.Kind != "type_parameter") || field.TypeId is null) return false;
            var substitutions = parameters.Zip(arguments).ToDictionary(pair => pair.First, pair => pair.Second, StringComparer.Ordinal);
            return SemanticGenericTypeSubstitution.TryClose(field.TypeId, substitutions, types, shapes, out var valueType)
                && operation.TypeId == valueType;
        }
    }
}

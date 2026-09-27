using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Validate serialized values without trusting the Roslyn projector or CLR layout.
public static class SemanticCancellationTokenValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document?.Methods is null || document.ControlFlowGraphs is null || document.AsyncMethods is null
            || document.Types is null || document.Symbols is null || document.Callables is null
            || document.TypeShapes is null || document.ClassTypes is null) return false;
        bool enabled = SemanticContract.HasCancellationTokens(document);
        if (!enabled && (document.SchemaVersion == SemanticContract.CancellationTokenSchemaVersion
            || document.SemanticVersion == SemanticContract.CancellationTokenSemanticVersion)) return false;
        if (enabled && (document.StaticInitialization is not null
            || document.Types.Any(type => type is null || string.IsNullOrWhiteSpace(type.Id))
            || document.Types.Select(type => type.Id).Distinct(StringComparer.Ordinal).Count() != document.Types.Count
            || document.Types.Count(type => type?.Id == SemanticCancellationTokens.TypeId
                && type.CanonicalName == "global::" + SemanticCancellationTokens.MetadataName
                && type.Kind == "struct" && type.IsValueType && !type.IsNullable) != 1
            || document.Types.Count(type => type?.Id == "type:int64" && type.CanonicalName == "int64"
                && type.Kind == "primitive" && type.IsValueType && !type.IsNullable) != 1
            || document.Types.Count(type => type?.Id == "type:bool" && type.CanonicalName == "bool"
                && type.Kind == "primitive" && type.IsValueType && !type.IsNullable) != 1
            || document.Symbols.Any(symbol => symbol?.Id == "symbol:type:global::" + SemanticCancellationTokens.MetadataName
                || symbol?.ContainingSymbolId == "symbol:type:global::" + SemanticCancellationTokens.MetadataName)
            || document.TypeShapes.Any(shape => shape?.TypeId == SemanticCancellationTokens.TypeId))) return false;

        // Visit each object once, with explicit cycle detection. A flat method
        // with thousands of ordinary calls must retain its existing limits;
        // token validation must not impose the async segment's node budget on it.
        var completed = new HashSet<SemanticOperation>(ReferenceEqualityComparer.Instance);
        bool Root(SemanticOperation? root)
        {
            var active = new HashSet<SemanticOperation>(ReferenceEqualityComparer.Instance);
            var pending = new Stack<(SemanticOperation? Node, bool Exit)>();
            pending.Push((root, false));
            while (pending.TryPop(out var entry))
            {
                var node = entry.Node;
                if (node?.Kind is null || node.Children is null) return false;
                if (entry.Exit)
                {
                    active.Remove(node);
                    completed.Add(node);
                    continue;
                }
                if (completed.Contains(node)) continue;
                if (!active.Add(node)) return false;
                if (node.Kind.StartsWith("cancellation_token_", StringComparison.Ordinal)
                    && (!enabled || !Canonical(node, document))) return false;
                pending.Push((node, true));
                foreach (var child in node.Children) pending.Push((child, false));
            }
            return true;
        }
        bool Roots(IReadOnlyList<SemanticOperation>? operations, SemanticOperation? branch) =>
            operations is not null && operations.All(Root) && (branch is null || Root(branch));
        foreach (var method in document.Methods)
            if (method is null || !Root(method.Root)) return false;
        foreach (var graph in document.ControlFlowGraphs)
            if (graph?.Blocks is null || graph.Blocks.Any(block => block is null
                || !Roots(block.Operations, block.BranchValue))) return false;
        foreach (var flow in document.ExceptionFlows ?? Array.Empty<SemanticExceptionFlow>())
            if (flow is null || flow.Blocks?.Any(block => block is null
                || !Roots(block.Operations, block.BranchValue)) == true) return false;
        foreach (var method in document.AsyncMethods)
        {
            if (method?.Segments is null) return false;
            foreach (var segment in method.Segments)
            {
                if (segment?.Statements is null || segment.Statements.Any(statement => statement is null || !Root(statement.Operation))
                    || segment.Transfer?.Condition is { } condition && !Root(condition)) return false;
                if (segment.AwaitSite is { } site && (!Roots(site.Arguments, site.CancellationToken)
                    || site.CancellationToken?.TypeId == SemanticCancellationTokens.TypeId && !enabled)) return false;
            }
        }
        return true;
    }

    private static bool Canonical(SemanticOperation node, SemanticDocument document)
    {
        if (!node.IsSupported || node.IsChecked || node.IsLifted || node.IsPostfix || node.IsTryCast
            || node.TypeArgumentIds is not { Count: 0 } || node.Constant is not null
            || node.Conversion is not null || node.InputConversion is not null || node.OutputConversion is not null
            || node.CaptureId is not null || node.Dispatch is not null || node.StaticFieldOwnerTypeId is not null
            || node.Span is not { Start: >= 0, Length: > 0 } || node.Children.Any(child => child is null)) return false;
        if (node.Kind == SemanticCancellationTokens.Compare)
            return node.TypeId == "type:bool" && node.SymbolId is null
                && node.OperatorKind is "equals" or "not_equals" && node.Children.Count == 2
                && node.Children.All(child => child.TypeId == SemanticCancellationTokens.TypeId);
        if (node.TypeId != SemanticCancellationTokens.TypeId || node.OperatorKind is not null) return false;
        return node.Kind switch
        {
            SemanticCancellationTokens.None => node.SymbolId is null && node.Children.Count == 0,
            SemanticCancellationTokens.Read => node.SymbolId is null && node.Children.Count == 1
                && IsCancellationException(node.Children[0].TypeId, document),
            SemanticCancellationTokens.FromAvid => node.Children.Count == 1
                && node.Children[0].TypeId == SemanticCancellationTokens.AvidTypeId
                && ValidConversion(node.SymbolId, document),
            _ => false,
        };
    }

    private static bool IsCancellationException(string? typeId, SemanticDocument document)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        while (typeId is not null && seen.Count < 128 && seen.Add(typeId))
        {
            if (document.Types.Count(type => type?.Id == typeId && type.Kind == "class" && !type.IsValueType) != 1) return false;
            var classes = document.ClassTypes.Where(type => type?.TypeId == typeId).ToArray();
            if (classes.Length != 1) return false;
            if (typeId == SemanticCancellationTokens.ExceptionTypeId) return !classes[0].IsSourceDeclared;
            typeId = classes[0].BaseTypeId;
        }
        return false;
    }

    private static bool ValidConversion(string? id, SemanticDocument document)
    {
        if (id is null) return false;
        var declarations = document.Callables.Where(callable => callable?.MethodSymbolId == id).ToArray();
        var symbols = document.Symbols.Where(symbol => symbol?.Id == id).ToArray();
        var fields = document.Symbols.Where(symbol => symbol is { Kind: "field", IsStatic: false }
            && symbol.ContainingSymbolId == "symbol:type:global::AvidScript.AvidCancellationToken").ToArray();
        return document.Types.Count(type => type?.Id == SemanticCancellationTokens.AvidTypeId
                && type.Kind == "struct" && type.IsValueType && !type.IsNullable) == 1
            && symbols is [{ Kind: "method", Name: "op_Implicit", IsStatic: true, IsExecutableReferenceSource: true,
                TypeId: SemanticCancellationTokens.TypeId, ContainingSymbolId: "symbol:type:global::AvidScript.AvidCancellationToken" }]
            && fields is [{ Name: "Value", TypeId: "type:int64", IsReadonly: true, IsExecutableReferenceSource: true }]
            && declarations is [{ IsStatic: true, IsConstructor: false, HasBody: false, Import: null,
                Export: null, AssociatedSymbolId: null, ContainingTypeId: SemanticCancellationTokens.AvidTypeId,
                ReturnTypeId: SemanticCancellationTokens.TypeId, Parameters: [{ Ordinal: 0, RefKind: "none",
                    TypeId: SemanticCancellationTokens.AvidTypeId }] }];
    }
}

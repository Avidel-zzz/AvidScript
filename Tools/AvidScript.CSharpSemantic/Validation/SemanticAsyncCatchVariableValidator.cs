using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// A binding is an explicit definition on the successful catch-dispatch edge.
// It represents the caught exception, not the result of invoking a constructor.
public static class SemanticAsyncCatchVariableValidator
{
    public const string BindingOperationKind = "async_caught_exception";

    public static bool IsValid(SemanticDocument document)
    {
        if (!SemanticCancellationTokenValidator.IsValid(document)
            || document?.AsyncMethods is null || document.Symbols is null
            || document.Symbols.Any(symbol => symbol is null || symbol.Span is null)) return false;
        bool enabled = SemanticContract.HasAsyncCatchVariables(document);
        if (!enabled && (document.SchemaVersion == SemanticContract.AsyncCatchVariableSchemaVersion
            || document.SemanticVersion == SemanticContract.AsyncCatchVariableSemanticVersion)) return false;
        var variables = new HashSet<string>(StringComparer.Ordinal);
        int bindings = 0;
        foreach (var method in document.AsyncMethods)
        {
            if (method?.Segments is null || method.Segments.Any(segment => segment?.Statements is null
                || segment.Statements.Any(statement => statement?.Operation is null))) return false;
            var operations = new Dictionary<int, List<SemanticOperation>>();
            foreach (var segment in method.Segments)
            {
                var roots = segment.Statements.Select(statement => statement.Operation);
                if (segment.Transfer?.Condition is { } condition) roots = roots.Append(condition);
                if (segment.AwaitSite is { } site)
                {
                    if (site.Arguments is null) return false;
                    roots = roots.Concat(site.Arguments);
                    if (site.CancellationToken is { } token) roots = roots.Append(token);
                }
                var nodes = new List<SemanticOperation>();
                if (roots.Any(root => !Collect(root, nodes, 0)) || !operations.TryAdd(segment.Ordinal, nodes)) return false;
            }
            var caught = operations.SelectMany(pair => pair.Value.Select(operation => (Segment: pair.Key, Operation: operation)))
                .Where(item => item.Operation.Kind == BindingOperationKind).ToArray();
            var handlers = method.ExceptionPlan?.Catches;
            if (method.ExceptionPlan is not null && (handlers is null || handlers.Any(handler => handler is null))) return false;
            var named = handlers?.Where(handler => handler.ExceptionVariableSymbolId is not null).ToArray()
                ?? Array.Empty<SemanticCatchHandler>();
            if (!enabled)
            {
                if (named.Length != 0 || caught.Length != 0) return false;
                continue;
            }
            if (caught.Length != named.Length) return false;
            foreach (var handler in named)
            {
                string variable = handler.ExceptionVariableSymbolId!;
                if (!variables.Add(variable) || handler.ExceptionTypeId is null || handler.HasFilter || handler.Span is null)
                    return false;
                var symbols = document.Symbols.Where(symbol => symbol.Id == variable).ToArray();
                var definitions = caught.Where(item => item.Operation.SymbolId == variable).ToArray();
                var regions = method.ExceptionPlan!.Regions?.Where(region => region?.RoslynRegionOrdinal == handler.RegionOrdinal).ToArray();
                if (symbols.Length != 1 || definitions.Length != 1 || regions is not { Length: 1 }
                    || regions[0] is not { Kind: "catch", Segments: not null } region || region.SourceSpan != handler.Span
                    || symbols[0] is not { Kind: "local", IsStatic: false, IsConst: false, IsExecutableReferenceSource: false } symbol
                    || symbol.ContainingSymbolId != method.MethodSymbolId || symbol.TypeId != handler.ExceptionTypeId
                    || symbol.Span.Start < handler.Span.Start || symbol.Span.Length <= 0
                    || (long)symbol.Span.Start + symbol.Span.Length > (long)handler.Span.Start + handler.Span.Length)
                    return false;
                var definition = definitions[0];
                var entry = method.Segments.SingleOrDefault(segment => segment.Ordinal == definition.Segment);
                if (entry is null || entry.Span != symbol.Span || !region.Segments.Contains(entry.Ordinal)
                    || entry.Statements.Count != 1 || entry.Statements[0].TargetSymbolId != variable
                    || entry.Statements[0].Operation != definition.Operation
                    || !Canonical(definition.Operation, symbol)
                    || entry.AwaitSite is not null || entry.SynchronousExceptionTarget is not null
                    || entry.Transfer is not { Kind: SemanticAsyncMethod.GotoTransferKind, Condition: null,
                        SecondaryTarget: -1, CancellationTarget: null, ExceptionTypeId: null } transfer
                    || transfer.PrimaryTarget == entry.Ordinal || !region.Segments.Contains(transfer.PrimaryTarget)
                    || method.Segments.Any(segment => segment.Transfer is null)) return false;
                var edges = SemanticAsyncScopeValidator.GetEntries(method.Segments, method.EntrySegmentOrdinal, region.Segments);
                if (edges.Length != 1 || edges[0].DestinationBlockOrdinal != entry.Ordinal
                    || edges[0].SourceBlockOrdinal is not int dispatch
                    || method.Segments.SingleOrDefault(segment => segment.Ordinal == dispatch)?.Transfer is not
                        { Kind: SemanticAsyncMethod.CatchMatchTransferKind } match
                    || match.PrimaryTarget != entry.Ordinal || match.ExceptionTypeId != handler.ExceptionTypeId
                    || region.Segments.Contains(match.SecondaryTarget)
                    || method.Segments.Any(segment => segment.Ordinal != dispatch
                        && SemanticAsyncScopeValidator.Successors(segment).Contains(entry.Ordinal))) return false;
                foreach (var segment in method.Segments)
                    if (!region.Segments.Contains(segment.Ordinal)
                        && (operations[segment.Ordinal].Any(operation => operation.SymbolId == variable)
                            || segment.Statements.Any(statement => statement.TargetSymbolId == variable))) return false;
                bindings++;
            }
        }
        return !enabled || bindings > 0 || SemanticContract.HasCancellationTokens(document)
            || SemanticContract.HasAsyncVoidErrorOwner(document)
            || SemanticObjectAwaitCancellation.Has(document);
    }

    private static bool Canonical(SemanticOperation operation, SemanticSymbol symbol) =>
        operation.IsSupported && operation.TypeId == symbol.TypeId && operation.Span == symbol.Span
        && operation.OperatorKind is null && !operation.IsChecked && !operation.IsLifted
        && !operation.IsPostfix && !operation.IsTryCast && operation.TypeArgumentIds is { Count: 0 }
        && operation.Constant is null && operation.Conversion is null && operation.InputConversion is null
        && operation.OutputConversion is null && operation.CaptureId is null && operation.Children is { Count: 0 }
        && operation.Dispatch is null && operation.StaticFieldOwnerTypeId is null;

    private static bool Collect(SemanticOperation? operation, List<SemanticOperation> nodes, int depth)
    {
        if (operation?.Children is null || depth > 128 || nodes.Count >= 16384) return false;
        nodes.Add(operation);
        return operation.Children.All(child => Collect(child, nodes, depth + 1));
    }
}

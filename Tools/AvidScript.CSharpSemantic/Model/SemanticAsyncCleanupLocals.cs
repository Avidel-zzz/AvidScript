using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace AvidScript.CSharpSemantic;

// Finally is copied for normal, return and exceptional exits. A copied source
// declaration has its own storage, constrained to the same lexical finally.
public static class SemanticAsyncCleanupLocals
{
    public static string Prefix(string methodId) => $"symbol:compiler_local:{methodId}:finally_local:";
    public static string Name(int blockStart, int copy, int declarationStart, string sourceName) =>
        $"<finally_local:{blockStart}:{copy}:{declarationStart}:{sourceName}>";

    public static bool IsDeclaration(SemanticAsyncStatement statement, string id) =>
        statement.TargetSymbolId == id || Operations(statement.Operation).Any(operation =>
            operation.Kind == SemanticAsyncMethod.LocalDeclarationOperationKind && operation.SymbolId == id);

    public static bool IsValid(SemanticDocument document, SemanticAsyncMethod method, SemanticAsyncCompilerLocal local)
    {
        if (!SemanticContract.HasAsyncSynchronousExceptions(document)
            || method.ExceptionPlan is null || !local.SymbolId.StartsWith(Prefix(method.MethodSymbolId), StringComparison.Ordinal)) return false;
        string[] parts = local.SymbolId[Prefix(method.MethodSymbolId).Length..].Split(':');
        if (parts.Length != 3 || !int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out int start)
            || !int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out int copy)
            || !int.TryParse(parts[2], NumberStyles.None, CultureInfo.InvariantCulture, out int declaration)
            || start > declaration || declaration != local.Span.Start) return false;
        var symbols = document.Symbols.Where(symbol => symbol.Kind == "local"
            && symbol.ContainingSymbolId == method.MethodSymbolId && symbol.TypeId == local.TypeId
            && Contains(local.Span, symbol.Span) && local.Name == Name(start, copy, declaration, symbol.Name)).ToArray();
        if (symbols.Length != 1) return false;
        var owners = method.ExceptionPlan.Regions.Where(region => region.Kind == "finally"
            && Contains(region.SourceSpan, local.Span) && region.SourceSpan.Start <= start).ToArray();
        if (owners.Length != 1) return false;
        int declarations = 0;
        foreach (var segment in method.Segments)
        {
            bool referenced = segment.Statements.Any(statement => statement.TargetSymbolId == local.SymbolId
                    || Operations(statement.Operation).Any(operation => operation.SymbolId == local.SymbolId))
                || segment.Transfer?.Condition is { } condition && Operations(condition).Any(operation => operation.SymbolId == local.SymbolId);
            if (referenced && !owners[0].Segments.Contains(segment.Ordinal)) return false;
            declarations += segment.Statements.Count(statement => IsDeclaration(statement, local.SymbolId));
        }
        return declarations == 1;
    }

    private static bool Contains(SemanticSpan outer, SemanticSpan inner) => outer.Start <= inner.Start && inner.End <= outer.End;
    private static IEnumerable<SemanticOperation> Operations(SemanticOperation operation) =>
        new[] { operation }.Concat(operation.Children.SelectMany(Operations));
}

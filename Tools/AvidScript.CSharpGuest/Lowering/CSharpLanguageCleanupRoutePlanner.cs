using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLanguageCleanupRegion(IReadOnlyList<string> BlockIds);
internal sealed record CSharpLanguageCleanupRoute(
    string SourceBlockId, IReadOnlyList<CSharpLanguageCleanupRegion> Regions)
{
    public IReadOnlyList<string> CleanupBlockIds =>
        Regions.SelectMany(region => region.BlockIds).ToArray();
}

// Identifies bounded, forward-only finally graphs that an exceptional direct
// call must execute. Calls that may raise a language error inside cleanup fail closed.
internal static class CSharpLanguageCleanupRoutePlanner
{
    public static bool TryBuild(
        SemanticDocument semantic,
        IReadOnlySet<string> affectedFunctionIds,
        out IReadOnlyDictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> routes,
        out string? error)
    {
        Dictionary<string, IReadOnlyList<CSharpLanguageCleanupRoute>> result =
            new(StringComparer.Ordinal);
        routes = result;
        error = null;
        IReadOnlySet<string> exceptionMethodIds = semantic.ExceptionFlows!
            .Select(flow => flow.MethodSymbolId).ToHashSet(StringComparer.Ordinal);
        foreach (SemanticMethodBody body in semantic.Methods)
        {
            string functionId = CSharpGuestIds.Function(body.MethodSymbolId);
            if (!affectedFunctionIds.Contains(functionId)
                || exceptionMethodIds.Contains(body.MethodSymbolId)) continue;
            List<SemanticOperation> operations = Descendants(body.Root).ToList();
            SemanticOperation[] tries = operations.Where(item => item.Kind == "try").ToArray();
            if (tries.Length == 0) continue;
            if (tries.Length > 16 || operations.Any(item => item.Kind is
                    "catch_clause" or "throw" or "await" or "loop")
                || tries.Any(item => item.Children.Count != 2
                    || item.Children[0].Kind != "block" || item.Children[1].Kind != "block"
                    || Descendants(item.Children[1]).Any(cleanup => cleanup.Kind is
                        "switch" or "branch" or "loop" or "try" or "await" or "throw")))
                return Fail($"Function '{body.MethodSymbolId}' needs unsupported error cleanup control flow.",
                    out error);
            SemanticControlFlowGraph[] graphs = semantic.ControlFlowGraphs.Where(item =>
                item.MethodSymbolId == body.MethodSymbolId).ToArray();
            if (graphs.Length != 1)
                return Fail($"Function '{body.MethodSymbolId}' has no unique cleanup CFG.", out error);
            SemanticControlFlowGraph graph = graphs[0];
            Dictionary<SemanticOperation, CSharpLanguageCleanupRegion> cleanupRegions = new();
            foreach (SemanticOperation statement in tries)
            {
                SemanticSpan span = statement.Children[1].Span;
                SemanticBasicBlock[] marked = graph.Blocks.Where(block =>
                    block.Operations.Any(operation => Contains(span, operation.Span))
                    || block.BranchValue is { } value && Contains(span, value.Span)).ToArray();
                if (marked.Length == 0)
                    return Fail($"Function '{body.MethodSymbolId}' has no source-backed finally block.", out error);
                int first = marked.Min(block => block.Ordinal);
                int last = marked.Max(block => block.Ordinal);
                SemanticBasicBlock[] candidates = graph.Blocks.Where(block =>
                    block.Ordinal >= first && block.Ordinal <= last)
                    .OrderBy(block => block.Ordinal).ToArray();
                if (candidates.Length != last - first + 1 || candidates.Length > 16
                    || candidates.Any(block => !block.IsReachable
                        || block.Operations.Any(operation => !Contains(span, operation.Span))
                        || block.BranchValue is { } value && !Contains(span, value.Span))
                    || !ValidForwardCleanup(candidates))
                    return Fail($"Function '{body.MethodSymbolId}' needs bounded forward finally cleanup.",
                        out error);
                cleanupRegions.Add(statement, new(candidates.Select(block =>
                    CSharpGuestIds.Block(body.MethodSymbolId, block.Ordinal)).ToArray()));
            }
            List<CSharpLanguageCleanupRoute> functionRoutes = new();
            foreach (SemanticBasicBlock block in graph.Blocks)
            {
                SemanticOperation[] calls = block.Operations
                    .SelectMany(Descendants)
                    .Concat(block.BranchValue is null
                        ? Array.Empty<SemanticOperation>() : Descendants(block.BranchValue))
                    .Where(operation => operation.Kind == "invocation"
                        && operation.SymbolId is { } symbolId
                        && affectedFunctionIds.Contains(CSharpGuestIds.Function(symbolId)))
                    .ToArray();
                if (calls.Length == 0) continue;
                CSharpLanguageCleanupRegion[][] paths = new CSharpLanguageCleanupRegion[calls.Length][];
                for (int index = 0; index < calls.Length; ++index)
                {
                    SemanticOperation call = calls[index];
                    if (tries.Any(statement => Contains(statement.Children[1].Span, call.Span)))
                        return Fail($"Function '{body.MethodSymbolId}' may throw while executing finally.",
                            out error);
                    paths[index] = tries.Where(statement =>
                            Contains(statement.Children[0].Span, call.Span))
                        .OrderBy(statement => statement.Children[0].Span.Length)
                        .Select(statement => cleanupRegions[statement]).ToArray();
                }
                if (paths.Skip(1).Any(path => !path.SequenceEqual(paths[0])))
                    return Fail($"Function '{body.MethodSymbolId}' has distinct cleanup scopes in one block.",
                        out error);
                functionRoutes.Add(new(CSharpGuestIds.Block(body.MethodSymbolId, block.Ordinal), paths[0]));
            }
            result.Add(functionId, functionRoutes);
        }
        return true;
    }

    private static IEnumerable<SemanticOperation> Descendants(SemanticOperation root)
    {
        Stack<SemanticOperation> pending = new();
        pending.Push(root);
        while (pending.TryPop(out SemanticOperation? operation))
        {
            yield return operation;
            for (int index = operation.Children.Count - 1; index >= 0; --index)
                pending.Push(operation.Children[index]);
        }
    }

    private static bool ValidForwardCleanup(IReadOnlyList<SemanticBasicBlock> blocks)
    {
        int first = blocks[0].Ordinal, last = blocks[^1].Ordinal;
        HashSet<int> visited = new() { first };
        int? exitTarget = null;
        foreach (SemanticBasicBlock block in blocks)
        {
            int ordinal = block.Ordinal;
            if (!visited.Contains(ordinal)
                || block.Predecessors.Any(edge => edge.Semantics != "regular"
                    || edge.DestinationBlockOrdinal != ordinal
                    || (edge.SourceBlockOrdinal < first
                        || edge.SourceBlockOrdinal > last) && ordinal != first)
                || block.Successors.Any(edge => edge.Semantics != "regular"
                    || edge.SourceBlockOrdinal != ordinal)) return false;
            if (block.Successors.Count == 2)
            {
                if (block.Operations.Count != 0 || block.ConditionKind == "none"
                    || block.BranchValue is not { } condition || !PureDecision(condition)
                    || block.Successors.Count(edge => edge.Kind == "conditional") != 1
                    || block.Successors.Count(edge => edge.Kind == "fallthrough") != 1
                    || block.Successors.Select(edge => edge.DestinationBlockOrdinal)
                        .Distinct().Count() != 2) return false;
            }
            else if (block.Successors.Count != 1 || block.ConditionKind != "none"
                || block.BranchValue is not null
                || block.Successors[0].Kind != "fallthrough") return false;
            foreach (SemanticControlFlowEdge edge in block.Successors)
            {
                int destination = edge.DestinationBlockOrdinal;
                if (destination >= first && destination <= last)
                {
                    if (destination <= ordinal) return false;
                    visited.Add(destination);
                }
                else if (exitTarget is null) exitTarget = destination;
                else if (exitTarget != destination) return false;
            }
        }
        return exitTarget is not null;
    }

    private static bool PureDecision(SemanticOperation operation)
    {
        if (!operation.IsSupported || operation.IsChecked || operation.IsLifted
            || (operation.Kind is "binary" or "unary") && operation.SymbolId is not null)
            return false;
        bool allowed = operation.Kind switch
        {
            "binary" => operation.OperatorKind is
                ("equals" or "not_equals" or "less_than" or "less_than_or_equal"
                    or "greater_than" or "greater_than_or_equal" or "logical_and"
                    or "logical_or" or "bitwise_and" or "bitwise_or" or "bitwise_xor"),
            "unary" => operation.OperatorKind is "logical_not" or "bitwise_not",
            "field_reference" or "local_reference" or "parameter_reference" or "literal" => true,
            _ => false,
        };
        return allowed && operation.Children.All(PureDecision);
    }

    private static bool Contains(SemanticSpan outer, SemanticSpan inner) =>
        outer.Start <= inner.Start && inner.End <= outer.End;

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}

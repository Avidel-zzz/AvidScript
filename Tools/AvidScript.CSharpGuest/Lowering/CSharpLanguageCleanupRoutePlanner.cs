using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpLanguageCleanupRoute(
    string SourceBlockId, IReadOnlyList<string> CleanupBlockIds);

// Identifies ordinary, already lowered finally blocks that an exceptional
// direct call must execute. Only linear synchronous cleanup can be copied;
// branching cleanup and a language-error call inside cleanup fail closed.
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
                        "conditional" or "switch" or "branch" or "loop" or "try" or "await")))
                return Fail($"Function '{body.MethodSymbolId}' needs unsupported error cleanup control flow.",
                    out error);
            SemanticControlFlowGraph[] graphs = semantic.ControlFlowGraphs.Where(item =>
                item.MethodSymbolId == body.MethodSymbolId).ToArray();
            if (graphs.Length != 1)
                return Fail($"Function '{body.MethodSymbolId}' has no unique cleanup CFG.", out error);
            SemanticControlFlowGraph graph = graphs[0];
            Dictionary<SemanticOperation, string> cleanupBlocks = new();
            foreach (SemanticOperation statement in tries)
            {
                SemanticSpan span = statement.Children[1].Span;
                SemanticBasicBlock[] candidates = graph.Blocks.Where(block =>
                    block.Operations.Count != 0 && block.BranchValue is null
                    && block.Operations.All(operation => Contains(span, operation.Span))
                    && block.Successors.Count == 1
                    && block.Successors[0].Semantics == "regular").ToArray();
                if (candidates.Length != 1)
                    return Fail($"Function '{body.MethodSymbolId}' needs one linear finally block.", out error);
                cleanupBlocks.Add(statement,
                    CSharpGuestIds.Block(body.MethodSymbolId, candidates[0].Ordinal));
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
                string[][] paths = new string[calls.Length][];
                for (int index = 0; index < calls.Length; ++index)
                {
                    SemanticOperation call = calls[index];
                    if (tries.Any(statement => Contains(statement.Children[1].Span, call.Span)))
                        return Fail($"Function '{body.MethodSymbolId}' may throw while executing finally.",
                            out error);
                    paths[index] = tries.Where(statement =>
                            Contains(statement.Children[0].Span, call.Span))
                        .OrderBy(statement => statement.Children[0].Span.Length)
                        .Select(statement => cleanupBlocks[statement]).ToArray();
                }
                if (paths.Skip(1).Any(path => !path.SequenceEqual(paths[0], StringComparer.Ordinal)))
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

    private static bool Contains(SemanticSpan outer, SemanticSpan inner) =>
        outer.Start <= inner.Start && inner.End <= outer.End;

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}

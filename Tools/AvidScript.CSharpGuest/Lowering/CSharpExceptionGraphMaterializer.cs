using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

// Internal bridge for exception methods whose ordinary block operations can be
// lowered unchanged. The exceptional call edges are added by the outcome pass.
internal static class CSharpExceptionGraphMaterializer
{
    public static bool TryBuild(SemanticExceptionFlow flow,
        out SemanticControlFlowGraph? graph, out string? error)
    {
        graph = null;
        error = null;
        if (flow.Throws.Count != 0 || flow.Catches.Count == 0
            || flow.Catches.Any(handler => handler.HasFilter
                || handler.ExceptionVariableSymbolId is not null)
            || flow.Regions.Any(region => region.Kind is not
                ("root" or "local_lifetime" or "try" or "try_and_catch" or "catch"))
            || flow.Blocks is not { Count: > 0 } blocks
            || flow.Branches.Any(branch => branch.DestinationBlockOrdinal < 0
                || branch.Semantics is not ("regular" or "return"))
            || blocks.Any(block => block.Operations.Any(operation => !Supported(operation))
                || block.BranchValue is { } value && !Supported(value)))
            return Fail("The catch method needs unsupported throw, cleanup, filter, variable, or block operations.", out error);

        SemanticControlFlowEdge[] edges = flow.Branches.Select(branch =>
            new SemanticControlFlowEdge(branch.SourceBlockOrdinal,
                branch.DestinationBlockOrdinal, branch.Kind, branch.Semantics)).ToArray();
        graph = new SemanticControlFlowGraph(flow.MethodSymbolId, 0, blocks.Count - 1,
            blocks.Select(block => new SemanticBasicBlock(block.Ordinal, block.Kind,
                block.IsReachable, block.ConditionKind, block.Operations, block.BranchValue,
                edges.Where(edge => edge.DestinationBlockOrdinal == block.Ordinal).ToArray(),
                edges.Where(edge => edge.SourceBlockOrdinal == block.Ordinal).ToArray()))
                .ToArray());
        return true;
    }

    private static bool Supported(SemanticOperation operation) =>
        operation.IsSupported && operation.Children.All(Supported);

    private static bool Fail(string message, out string? error)
    {
        error = message;
        return false;
    }
}

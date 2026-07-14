using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpControlFlowLowerer
{
    public static GuestFunction? Lower(
        SemanticDocument document,
        SemanticCallable callable,
        SemanticControlFlowGraph graph,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        CSharpGuestDataPool dataPool,
        List<GuestDiagnostic> diagnostics)
    {
        int functionDiagnosticStart = diagnostics.Count;
        List<GuestRegister> parameters = LowerParameters(callable, guestTypes, diagnostics);
        if (diagnostics.Count != functionDiagnosticStart)
        {
            return null;
        }

        CSharpFunctionLoweringContext context = new(
            document,
            callable,
            guestTypes,
            dataPool,
            parameters,
            diagnostics);
        int initialDiagnosticCount = functionDiagnosticStart;
        List<GuestBasicBlock> blocks = new();
        foreach (SemanticBasicBlock block in graph.Blocks.OrderBy(item => item.Ordinal))
        {
            List<GuestInstruction> instructions = new();
            foreach (SemanticOperation operation in block.Operations)
            {
                CSharpOperationLowerer.LowerValue(context, operation, block.Ordinal, instructions);
            }

            GuestTerminator? terminator = LowerTerminator(context, graph, block, instructions);
            if (terminator is null || diagnostics.Count != initialDiagnosticCount)
            {
                return null;
            }

            blocks.Add(new GuestBasicBlock(
                CSharpGuestIds.Block(callable.MethodSymbolId, block.Ordinal),
                instructions,
                terminator));
        }

        return new GuestFunction(
            CSharpGuestIds.Function(callable.MethodSymbolId),
            parameters,
            context.Locals,
            callable.ReturnTypeId,
            CSharpGuestIds.Block(callable.MethodSymbolId, graph.EntryBlockOrdinal),
            blocks);
    }

    private static List<GuestRegister> LowerParameters(
        SemanticCallable callable,
        IReadOnlyDictionary<string, GuestType> guestTypes,
        List<GuestDiagnostic> diagnostics)
    {
        List<GuestRegister> parameters = new();
        if (!callable.IsStatic)
        {
            if (!guestTypes.ContainsKey(callable.ContainingTypeId))
            {
                Add(diagnostics, "ASCG1004", callable.MethodSymbolId,
                    $"Instance callable containing type '{callable.ContainingTypeId}' has no Guest representation.");
                return parameters;
            }

            parameters.Add(new GuestRegister(
                CSharpGuestIds.This(callable.MethodSymbolId),
                callable.ContainingTypeId));
        }

        foreach (SemanticCallableParameter parameter in callable.Parameters.OrderBy(item => item.Ordinal))
        {
            string typeId = CSharpAbiTypeMapper.ParameterType(parameter);
            if (!guestTypes.ContainsKey(typeId))
            {
                Add(diagnostics, "ASCG1004", callable.MethodSymbolId,
                    $"Parameter '{parameter.SymbolId}' type '{typeId}' has no Guest representation.");
                return parameters;
            }

            parameters.Add(new GuestRegister(CSharpGuestIds.Parameter(parameter.SymbolId), typeId));
        }

        return parameters;
    }

    private static GuestTerminator? LowerTerminator(
        CSharpFunctionLoweringContext context,
        SemanticControlFlowGraph graph,
        SemanticBasicBlock block,
        List<GuestInstruction> instructions)
    {
        SemanticControlFlowEdge[] returnEdges = block.Successors
            .Where(edge => edge.Semantics == "return")
            .ToArray();
        if (returnEdges.Length != 0)
        {
            if (returnEdges.Length != 1 || block.Successors.Count != 1)
            {
                context.Add("ASCG1004", $"Block {block.Ordinal} has ambiguous return control flow.");
                return null;
            }

            bool returnsVoid = string.Equals(
                context.Callable.ReturnTypeId,
                "type:void",
                StringComparison.Ordinal);
            if (returnsVoid)
            {
                if (block.BranchValue is not null)
                {
                    context.Add("ASCG1004", $"Void return block {block.Ordinal} carries a value.");
                    return null;
                }

                return new GuestTerminator("return", null, null, null, null);
            }

            if (block.BranchValue is null)
            {
                context.Add("ASCG1004", $"Non-void return block {block.Ordinal} has no branch value.");
                return null;
            }

            GuestRegister? returnValue = CSharpOperationLowerer.LowerValue(
                context,
                block.BranchValue,
                block.Ordinal,
                instructions);
            return returnValue is null
                ? null
                : new GuestTerminator("return", null, null, null, returnValue.Id);
        }

        SemanticControlFlowEdge[] regularEdges = block.Successors
            .Where(edge => edge.Semantics == "regular")
            .ToArray();
        if (block.BranchValue is not null)
        {
            return LowerConditional(context, block, regularEdges, instructions);
        }

        if (regularEdges.Length == 1 && block.Successors.Count == 1)
        {
            return new GuestTerminator(
                "branch",
                null,
                CSharpGuestIds.Block(context.Callable.MethodSymbolId, regularEdges[0].DestinationBlockOrdinal),
                null,
                null);
        }

        if (block.Ordinal == graph.ExitBlockOrdinal && block.Successors.Count == 0)
        {
            return new GuestTerminator("trap", null, null, null, null);
        }

        context.Add("ASCG1004", $"Block {block.Ordinal} has unsupported control flow.");
        return null;
    }

    private static GuestTerminator? LowerConditional(
        CSharpFunctionLoweringContext context,
        SemanticBasicBlock block,
        IReadOnlyList<SemanticControlFlowEdge> edges,
        List<GuestInstruction> instructions)
    {
        if (edges.Count != 2 || block.Successors.Count != 2)
        {
            context.Add("ASCG1004", $"Conditional block {block.Ordinal} does not have two regular edges.");
            return null;
        }

        SemanticControlFlowEdge[] fallthroughEdges = edges
            .Where(edge => edge.Kind == "fallthrough")
            .ToArray();
        SemanticControlFlowEdge[] conditionalEdges = edges
            .Where(edge => edge.Kind == "conditional")
            .ToArray();
        if (fallthroughEdges.Length != 1 || conditionalEdges.Length != 1)
        {
            context.Add("ASCG1004", $"Conditional block {block.Ordinal} has ambiguous edge kinds.");
            return null;
        }

        SemanticControlFlowEdge fallthrough = fallthroughEdges[0];
        SemanticControlFlowEdge conditional = conditionalEdges[0];

        GuestRegister? condition = CSharpOperationLowerer.LowerValue(
            context,
            block.BranchValue!,
            block.Ordinal,
            instructions);
        if (condition is null)
        {
            return null;
        }

        SemanticControlFlowEdge trueEdge;
        SemanticControlFlowEdge falseEdge;
        switch (block.ConditionKind)
        {
            case "when_false":
                trueEdge = fallthrough;
                falseEdge = conditional;
                break;
            case "when_true":
                trueEdge = conditional;
                falseEdge = fallthrough;
                break;
            default:
                context.Add("ASCG1004", $"Conditional block {block.Ordinal} has unknown condition kind '{block.ConditionKind}'.");
                return null;
        }

        return new GuestTerminator(
            "branch_if",
            condition.Id,
            CSharpGuestIds.Block(context.Callable.MethodSymbolId, trueEdge.DestinationBlockOrdinal),
            CSharpGuestIds.Block(context.Callable.MethodSymbolId, falseEdge.DestinationBlockOrdinal),
            null);
    }

    private static void Add(
        List<GuestDiagnostic> diagnostics,
        string code,
        string methodId,
        string message)
    {
        diagnostics.Add(new GuestDiagnostic(code, "error", $"{methodId}: {message}", null));
    }
}
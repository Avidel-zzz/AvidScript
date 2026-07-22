using System;
using System.Collections.Generic;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpCallOperationLowerer
{
    public static GuestRegister? LowerInvocation(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!context.TryGetCallTarget(operation.SymbolId, out SemanticCallable callable, out string targetId))
        {
            context.Add("ASCG1005", $"Block {blockOrdinal} call target '{operation.SymbolId}' is not an import or Guest function.");
            return null;
        }

        if (!TryLowerOperands(context, callable, operation.Children, blockOrdinal, instructions, out List<string> operands))
        {
            return null;
        }

        return CSharpOperationLowerer.EmitCall(
            context,
            callable,
            targetId,
            operands,
            blockOrdinal,
            instructions);
    }

    public static GuestRegister? LowerProperty(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!context.TryGetPropertyGetter(operation.SymbolId, out SemanticCallable callable, out string targetId))
        {
            context.Add("ASCG1005", $"Block {blockOrdinal} property '{operation.SymbolId}' has no Guest getter.");
            return null;
        }

        if (!TryLowerOperands(context, callable, operation.Children, blockOrdinal, instructions, out List<string> operands))
        {
            return null;
        }

        return CSharpOperationLowerer.EmitCall(
            context,
            callable,
            targetId,
            operands,
            blockOrdinal,
            instructions);
    }

    public static GuestRegister? LowerObjectCreation(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (CSharpClassReferenceLowerer.TryLowerObjectCreation(
                context,
                operation,
                blockOrdinal,
                instructions,
                out GuestRegister? classReference))
        {
            return classReference;
        }

        if (!context.TryGetCallTarget(operation.SymbolId, out SemanticCallable constructor, out string targetId)
            || !constructor.IsConstructor
            || constructor.IsStatic)
        {
            context.Add("ASCG1005", $"Block {blockOrdinal} constructor '{operation.SymbolId}' is not a Guest constructor.");
            return null;
        }

        GuestRegister? instance = CSharpAggregateOperationLowerer.Allocate(
            context,
            operation.TypeId,
            blockOrdinal,
            instructions);
        if (instance is null)
        {
            return null;
        }

        if (!TryLowerArguments(
                context,
                constructor.Parameters,
                operation.Children,
                blockOrdinal,
                instructions,
                out List<string> arguments))
        {
            return null;
        }

        arguments.Insert(0, instance.Id);
        CSharpOperationLowerer.EmitCall(
            context,
            constructor,
            targetId,
            arguments,
            blockOrdinal,
            instructions);
        return instance;
    }

    private static bool TryLowerOperands(
        CSharpFunctionLoweringContext context,
        SemanticCallable callable,
        IReadOnlyList<SemanticOperation> children,
        int blockOrdinal,
        List<GuestInstruction> instructions,
        out List<string> operands)
    {
        operands = new List<string>();
        int argumentStart = 0;
        if (!callable.IsStatic)
        {
            if (children.Count == 0)
            {
                context.Add("ASCG1004", $"Block {blockOrdinal} instance call '{callable.MethodSymbolId}' has no receiver.");
                return false;
            }

            GuestRegister? receiver = CSharpOperationLowerer.LowerValue(
                context,
                children[0],
                blockOrdinal,
                instructions);
            if (receiver is null)
            {
                return false;
            }

            operands.Add(receiver.Id);
            argumentStart = 1;
        }

        IReadOnlyList<SemanticOperation> arguments = children.Count == argumentStart
            ? Array.Empty<SemanticOperation>()
            : new ArraySegment<SemanticOperation>(
                children is SemanticOperation[] array ? array : new List<SemanticOperation>(children).ToArray(),
                argumentStart,
                children.Count - argumentStart);
        if (!TryLowerArguments(
                context,
                callable.Parameters,
                arguments,
                blockOrdinal,
                instructions,
                out List<string> loweredArguments))
        {
            return false;
        }

        operands.AddRange(loweredArguments);
        return true;
    }

    private static bool TryLowerArguments(
        CSharpFunctionLoweringContext context,
        IReadOnlyList<SemanticCallableParameter> parameters,
        IReadOnlyList<SemanticOperation> arguments,
        int blockOrdinal,
        List<GuestInstruction> instructions,
        out List<string> operands)
    {
        operands = new List<string>();
        if (parameters.Count != arguments.Count)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} call argument count does not match semantic callable.");
            return false;
        }

        for (int index = 0; index < parameters.Count; ++index)
        {
            SemanticOperation argument = arguments[index];
            SemanticOperation value = argument.Kind == "argument" && argument.Children.Count == 1
                ? argument.Children[0]
                : argument;
            GuestRegister? operand = parameters[index].RefKind == "none"
                ? CSharpOperationLowerer.LowerValue(context, value, blockOrdinal, instructions)
                : CSharpOperationLowerer.LowerAddress(context, value, blockOrdinal, instructions);
            if (operand is null)
            {
                return false;
            }

            operands.Add(operand.Id);
        }

        return true;
    }
}

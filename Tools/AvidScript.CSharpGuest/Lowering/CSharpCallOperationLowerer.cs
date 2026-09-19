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
        if (operation.Dispatch?.Kind is "virtual" or "interface")
        {
            context.Add("ASCG1024", "Virtual/interface dispatch requires a runtime method route; a fixed Guest body is not equivalent.");
            return null;
        }
        if (CSharpReferenceObjectCreation.IsRootInitializer(context, operation)) return null;
        if (CSharpManagedDelegateLowerer.TryLowerInvocation(context, operation, blockOrdinal, instructions, out GuestRegister? delegateResult))
            return delegateResult;
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
        if (context.IsUeProperty(operation.SymbolId))
        {
            GuestRegister? receiver = LowerPropertyReceiver(
                context,
                operation,
                blockOrdinal,
                instructions);
            return receiver is null
                ? null
                : LowerProperty(context, operation, receiver, blockOrdinal, instructions);
        }

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

    public static GuestRegister? LowerProperty(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        GuestRegister receiver,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!context.TryGetPropertyGetter(operation.SymbolId, out SemanticCallable callable, out string targetId))
        {
            context.Add("ASCG1005", $"Block {blockOrdinal} property '{operation.SymbolId}' has no Guest getter.");
            return null;
        }

        if (callable.IsStatic || callable.Parameters.Count != 0 || operation.Children.Count != 1)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} property getter '{operation.SymbolId}' has an invalid receiver contract.");
            return null;
        }

        GuestRegister? normalizedReceiver = context.NormalizeUeReceiver(
            callable,
            receiver,
            blockOrdinal,
            instructions);
        if (normalizedReceiver is null)
        {
            return null;
        }

        return CSharpOperationLowerer.EmitCall(
            context,
            callable,
            targetId,
            new[] { normalizedReceiver.Id },
            blockOrdinal,
            instructions);
    }

    public static GuestRegister? LowerPropertyReceiver(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (operation.Children.Count != 1)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} property '{operation.SymbolId}' has an invalid receiver contract.");
            return null;
        }

        if ((context.TryGetPropertyGetter(operation.SymbolId, out SemanticCallable accessor, out _)
                || context.TryGetPropertySetter(operation.SymbolId, out accessor, out _))
            && CSharpBorrowedReferences.BorrowedReceiver(context, accessor))
            return CSharpBorrowedReferences.Receiver(context, operation.Children[0], blockOrdinal, instructions, accessor);
        if (context.ClosureCells.RejectValueReceiverBorrow(operation.Children[0])) return null;
        return CSharpOperationLowerer.LowerValue(
            context,
            operation.Children[0],
            blockOrdinal,
            instructions);
    }

    public static bool LowerPropertySetter(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        GuestRegister value,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        GuestRegister? receiver = LowerPropertyReceiver(
            context,
            operation,
            blockOrdinal,
            instructions);
        return receiver is not null && LowerPropertySetter(
            context,
            operation,
            receiver,
            value,
            blockOrdinal,
            instructions);
    }

    public static bool LowerPropertySetter(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        GuestRegister receiver,
        GuestRegister value,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (!context.TryGetPropertySetter(operation.SymbolId, out SemanticCallable callable, out string targetId))
        {
            context.Add("ASCG1005", $"Block {blockOrdinal} property '{operation.SymbolId}' has no Guest setter.");
            return false;
        }

        if (callable.IsStatic || callable.Parameters.Count != 1 || operation.Children.Count != 1)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} property setter '{operation.SymbolId}' has an invalid receiver contract.");
            return false;
        }

        GuestRegister? normalizedReceiver = context.NormalizeUeReceiver(
            callable,
            receiver,
            blockOrdinal,
            instructions);
        if (normalizedReceiver is null)
        {
            return false;
        }

        CSharpOperationLowerer.EmitCall(
            context,
            callable,
            targetId,
            new[] { normalizedReceiver.Id, value.Id },
            blockOrdinal,
            instructions);
        return true;
    }

    public static GuestRegister? LowerObjectCreation(
        CSharpFunctionLoweringContext context,
        SemanticOperation operation,
        int blockOrdinal,
        List<GuestInstruction> instructions)
    {
        if (CSharpObjectCapabilityLowerer.TryLowerObjectCreation(
                context,
                operation,
                blockOrdinal,
                instructions,
                out GuestRegister? objectCapability))
        {
            return objectCapability;
        }

        if (CSharpClassReferenceLowerer.TryLowerObjectCreation(
                context,
                operation,
                blockOrdinal,
                instructions,
                out GuestRegister? classReference))
        {
            return classReference;
        }

        if (CSharpReferenceObjects.Types(context.Document).Contains(operation.TypeId ?? ""))
            return CSharpReferenceObjectCreation.Lower(context, operation, blockOrdinal, instructions);
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
                out List<string> arguments, CSharpBorrowedReferences.Enabled(context.Document)))
        {
            return null;
        }

        GuestRegister? constructorReceiver = CSharpBorrowedReferences.BorrowedReceiver(context, constructor)
            ? CSharpBorrowedReferences.Storage(context, instance, blockOrdinal, instructions) : instance;
        if (constructorReceiver is null) return null;
        arguments.Insert(0, constructorReceiver.Id);
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

            bool borrowedReceiver = CSharpBorrowedReferences.BorrowedReceiver(context, callable);
            if (!borrowedReceiver && context.ClosureCells.RejectValueReceiverBorrow(children[0])) return false;
            GuestRegister? receiver = borrowedReceiver
                ? CSharpBorrowedReferences.Receiver(context, children[0], blockOrdinal, instructions, callable)
                : CSharpOperationLowerer.LowerValue(
                context,
                children[0],
                blockOrdinal,
                instructions);
            if (receiver is null)
            {
                return false;
            }

            receiver = context.NormalizeUeReceiver(
                callable,
                receiver,
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
                out List<string> loweredArguments, callable.Import is null && CSharpBorrowedReferences.Enabled(context.Document)))
        {
            return false;
        }

        operands.AddRange(loweredArguments);
        return true;
    }

    internal static bool TryLowerArguments(
        CSharpFunctionLoweringContext context,
        IReadOnlyList<SemanticCallableParameter> parameters,
        IReadOnlyList<SemanticOperation> arguments,
        int blockOrdinal,
        List<GuestInstruction> instructions,
        out List<string> operands,
        bool borrowed = false)
    {
        operands = new List<string>();
        if (parameters.Count != arguments.Count)
        {
            context.Add("ASCG1004", $"Block {blockOrdinal} call argument count does not match semantic callable.");
            return false;
        }

        for (int index = 0; index < parameters.Count; ++index)
        {
            if (CSharpClosureLayout.CapturedParameter(context.Document, parameters[index].SymbolId) is { } captured)
            {
                GuestRegister? environment = context.ClosureCells.Environment(captured.Environment.Id);
                if (environment is null) { context.Add("ASCG1024", "Direct captured call cannot resolve its shared environment."); return false; }
                operands.Add(environment.Id); continue;
            }
            SemanticOperation argument = arguments[index];
            SemanticOperation value = argument.Kind == "argument" && argument.Children.Count == 1
                ? argument.Children[0]
                : argument;
            GuestRegister? operand = parameters[index].RefKind == "none"
                ? CSharpOperationLowerer.LowerValue(context, value, blockOrdinal, instructions)
                : CSharpOperationLowerer.LowerAddress(context, value, blockOrdinal, instructions, borrowed);
            if (operand is null)
            {
                return false;
            }

            operands.Add(operand.Id);
        }

        return true;
    }
}

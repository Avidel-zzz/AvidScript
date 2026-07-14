using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestPointerInstructionValidator
{
    private const string AddressTypeId = "type:address";

    public static void ValidateLoad(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        if (result is null
            || operands.Count != 1
            || operands[0] is null
            || !string.Equals(operands[0]!.TypeId, AddressTypeId, StringComparison.Ordinal)
            || instruction.TargetId is null
            || !context.Types.ContainsKey(instruction.TargetId)
            || !string.Equals(result.TypeId, instruction.TargetId, StringComparison.Ordinal)
            || instruction.OperatorKind is not null
            || instruction.Constant is not null)
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateStore(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        if (result is not null
            || operands.Count != 2
            || operands[0] is null
            || operands[1] is null
            || !string.Equals(operands[0]!.TypeId, AddressTypeId, StringComparison.Ordinal)
            || instruction.TargetId is null
            || !context.Types.ContainsKey(instruction.TargetId)
            || !string.Equals(operands[1]!.TypeId, instruction.TargetId, StringComparison.Ordinal)
            || instruction.OperatorKind is not null
            || instruction.Constant is not null)
        {
            Add(context, function, instruction);
        }
    }

    private static void Add(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction)
    {
        context.Add(
            "ASIR1008",
            $"Function '{function.Id}' pointer instruction '{instruction.Op}' has invalid address or pointee type.");
    }
}

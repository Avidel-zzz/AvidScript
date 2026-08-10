using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestArrayInstructionValidator
{
    public static void ValidateLength(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        bool valid = instruction.OperatorKind is null
            && instruction.Constant is null
            && instruction.TargetId is null
            && result is not null
            && operands.Count == 1
            && operands[0] is not null
            && context.Types.TryGetValue(operands[0]!.TypeId, out GuestType? arrayType)
            && string.Equals(arrayType.Kind, "array", StringComparison.Ordinal)
            && context.Types.TryGetValue(result.TypeId, out GuestType? resultType)
            && string.Equals(resultType.Kind, "scalar", StringComparison.Ordinal)
            && string.Equals(resultType.Storage, "i32", StringComparison.Ordinal)
            && resultType.Size == 4;
        if (!valid)
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateLoad(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        bool valid = TryResolveShape(
            context,
            instruction,
            operands,
            out GuestType? elementType)
            && result is not null
            && operands.Count == 2
            && string.Equals(result.TypeId, elementType!.Id, StringComparison.Ordinal);
        if (!valid)
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
        bool valid = TryResolveShape(
            context,
            instruction,
            operands,
            out GuestType? elementType)
            && result is null
            && operands.Count == 3
            && operands[2] is not null
            && string.Equals(operands[2]!.TypeId, elementType!.Id, StringComparison.Ordinal);
        if (!valid)
        {
            Add(context, function, instruction);
        }
    }

    private static bool TryResolveShape(
        GuestValidationContext context,
        GuestInstruction instruction,
        IReadOnlyList<GuestRegister?> operands,
        out GuestType? elementType)
    {
        elementType = null;
        if (instruction.OperatorKind is not null
            || instruction.Constant is not null
            || instruction.TargetId is null
            || operands.Count < 2
            || operands[0] is null
            || operands[1] is null
            || !context.Types.TryGetValue(operands[0]!.TypeId, out GuestType? arrayType)
            || !string.Equals(arrayType.Kind, "array", StringComparison.Ordinal)
            || arrayType.ElementTypeId is null
            || !string.Equals(arrayType.ElementTypeId, instruction.TargetId, StringComparison.Ordinal)
            || !context.Types.TryGetValue(arrayType.ElementTypeId, out elementType)
            || !context.Types.TryGetValue(operands[1]!.TypeId, out GuestType? indexType)
            || indexType.Kind is not ("scalar" or "enum")
            || !string.Equals(indexType.Storage, "i32", StringComparison.Ordinal)
            || indexType.Size != 4)
        {
            return false;
        }

        return true;
    }

    private static void Add(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction)
    {
        context.Add(
            "ASIR1008",
            $"Function '{function.Id}' array instruction '{instruction.Op}' has invalid array, index, or element types.");
    }
}

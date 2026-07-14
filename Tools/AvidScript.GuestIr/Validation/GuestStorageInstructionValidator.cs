using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestStorageInstructionValidator
{
    public static void ValidateLocalLoad(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands,
        IReadOnlyDictionary<string, GuestRegister> values)
    {
        if (result is null
            || instruction.TargetId is null
            || operands.Count != 0
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || !values.TryGetValue(instruction.TargetId, out GuestRegister? target)
            || !string.Equals(result.TypeId, target.TypeId, StringComparison.Ordinal))
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateLocalStore(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands,
        IReadOnlyDictionary<string, GuestRegister> values)
    {
        if (result is not null
            || instruction.TargetId is null
            || operands.Count != 1
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || !values.TryGetValue(instruction.TargetId, out GuestRegister? target)
            || operands[0] is null
            || !string.Equals(operands[0]!.TypeId, target.TypeId, StringComparison.Ordinal))
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateGlobalLoad(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        GuestGlobal? global = instruction.TargetId is null
            ? null
            : context.Module.Globals.FirstOrDefault(item => item.Id == instruction.TargetId);
        if (result is null
            || global is null
            || operands.Count != 0
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || !string.Equals(result.TypeId, global.TypeId, StringComparison.Ordinal))
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateGlobalStore(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        GuestGlobal? global = instruction.TargetId is null
            ? null
            : context.Module.Globals.FirstOrDefault(item => item.Id == instruction.TargetId);
        if (result is not null
            || global is null
            || operands.Count != 1
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || operands[0] is null
            || !string.Equals(operands[0]!.TypeId, global.TypeId, StringComparison.Ordinal))
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
            $"Function '{function.Id}' storage instruction '{instruction.Op}' has invalid target or types.");
    }
}

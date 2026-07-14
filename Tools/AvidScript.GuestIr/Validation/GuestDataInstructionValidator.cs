using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestDataInstructionValidator
{
    public static void ValidateAddress(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        GuestDataSegment? segment = instruction.TargetId is null
            ? null
            : context.Module.DataSegments.FirstOrDefault(item => item.Id == instruction.TargetId);
        if (result is null
            || segment is null
            || operands.Count != 0
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || !string.Equals(result.TypeId, segment.TypeId, StringComparison.Ordinal))
        {
            context.Add(
                "ASIR1008",
                $"Function '{function.Id}' data address instruction has invalid target or type.");
        }
    }
}

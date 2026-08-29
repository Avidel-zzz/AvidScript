using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestAggregateInstructionValidator
{
    private const string AddressTypeId = "type:address";

    public static void ValidateStackAlloc(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        if (result is null
            || operands.Count != 0
            || instruction.TargetId is not null
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || !context.Types.TryGetValue(result.TypeId, out GuestType? type)
            || type.Kind != "struct")
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateFieldLoad(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        GuestField? field = operands.Count == 1 && operands[0] is not null
            ? ResolveField(context, operands[0]!.TypeId, instruction.TargetId)
            : null;
        if (result is null
            || field is null
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || !string.Equals(result.TypeId, field.TypeId, StringComparison.Ordinal))
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateFieldStore(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        GuestField? field = operands.Count == 2 && operands[0] is not null
            ? ResolveField(context, operands[0]!.TypeId, instruction.TargetId)
            : null;
        if (result is not null
            || field is null
            || operands[1] is null
            || instruction.OperatorKind is not null
            || instruction.Constant is not null
            || !string.Equals(operands[1]!.TypeId, field.TypeId, StringComparison.Ordinal))
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateMemoryCopy(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        GuestType? type = instruction.TargetId is { } typeId
            && context.Types.TryGetValue(typeId, out GuestType? resolved)
                ? resolved
                : null;
        if (result is not null
            || operands.Count != 2
            || operands[0] is null
            || operands[1] is null
            || type is null
            || type.Storage != "memory"
            || type.Size <= 0
            || operands[0]!.TypeId != type.Id
            || operands[1]!.TypeId != type.Id
            || instruction.OperatorKind is not null
            || instruction.Constant is not null)
        {
            Add(context, function, instruction);
        }
    }

    public static void ValidateAddressOf(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands,
        IReadOnlyDictionary<string, GuestRegister> values)
    {
        if (result is null
            || !string.Equals(result.TypeId, AddressTypeId, StringComparison.Ordinal)
            || operands.Count != 0
            || instruction.TargetId is null
            || !values.ContainsKey(instruction.TargetId)
            || instruction.OperatorKind is not null
            || instruction.Constant is not null)
        {
            Add(context, function, instruction);
        }
    }

    private static GuestField? ResolveField(
        GuestValidationContext context,
        string aggregateTypeId,
        string? fieldId)
    {
        return fieldId is not null
            && context.Types.TryGetValue(aggregateTypeId, out GuestType? aggregate)
            && aggregate.Kind == "struct"
                ? aggregate.Fields.FirstOrDefault(field => field.Id == fieldId)
                : null;
    }

    private static void Add(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction)
    {
        context.Add(
            "ASIR1008",
            $"Function '{function.Id}' aggregate instruction '{instruction.Op}' has invalid operands or metadata.");
    }
}

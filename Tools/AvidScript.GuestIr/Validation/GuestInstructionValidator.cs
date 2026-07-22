using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestInstructionValidator
{
    public static void Validate(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        IReadOnlyDictionary<string, GuestRegister> values,
        IReadOnlySet<string> localIds,
        HashSet<string> assignedValues)
    {
        GuestRegister? result = ResolveResult(context, function, instruction, values, localIds, assignedValues);
        GuestRegister?[] operands = ResolveOperands(context, function, instruction, values);

        switch (instruction.Op)
        {
            case "constant":
                ValidateConstantInstruction(context, function, instruction, result);
                break;
            case "copy":
                ValidateCopy(context, function, instruction, result, operands);
                break;
            case "binary":
                ValidateBinary(context, function, instruction, result, operands);
                break;
            case "convert":
                ValidateConvert(context, function, instruction, result, operands);
                break;
            case "call":
                ValidateCall(context, function, instruction, result, operands);
                break;
            case "local_load":
                GuestStorageInstructionValidator.ValidateLocalLoad(
                    context, function, instruction, result, operands, values);
                break;
            case "local_store":
                GuestStorageInstructionValidator.ValidateLocalStore(
                    context, function, instruction, result, operands, values);
                break;
            case "global_load":
                GuestStorageInstructionValidator.ValidateGlobalLoad(
                    context, function, instruction, result, operands);
                break;
            case "global_store":
                GuestStorageInstructionValidator.ValidateGlobalStore(
                    context, function, instruction, result, operands);
                break;
            case "stack_alloc":
                GuestAggregateInstructionValidator.ValidateStackAlloc(
                    context, function, instruction, result, operands);
                break;
            case "field_load":
                GuestAggregateInstructionValidator.ValidateFieldLoad(
                    context, function, instruction, result, operands);
                break;
            case "field_store":
                GuestAggregateInstructionValidator.ValidateFieldStore(
                    context, function, instruction, result, operands);
                break;
            case "address_of":
                GuestAggregateInstructionValidator.ValidateAddressOf(
                    context, function, instruction, result, operands, values);
                break;
            case "data_address":
                GuestDataInstructionValidator.ValidateAddress(
                    context, function, instruction, result, operands);
                break;
            case "array_load":
                GuestArrayInstructionValidator.ValidateLoad(
                    context, function, instruction, result, operands);
                break;
            case "array_store":
                GuestArrayInstructionValidator.ValidateStore(
                    context, function, instruction, result, operands);
                break;
            case "indirect_load":
                GuestPointerInstructionValidator.ValidateLoad(
                    context, function, instruction, result, operands);
                break;
            case "indirect_store":
                GuestPointerInstructionValidator.ValidateStore(
                    context, function, instruction, result, operands);
                break;
            default:
                context.Add(
                    "ASIR1011",
                    $"Function '{function.Id}' uses unsupported instruction '{instruction.Op}'.");
                break;
        }
    }

    public static void ValidateConstant(
        GuestValidationContext context,
        GuestConstant constant,
        GuestType targetType,
        string owner)
    {
        if (!GuestConstantCodec.TryEncode(constant, targetType, out _))
        {
            context.Add(
                "ASIR1008",
                $"{owner} constant kind '{constant.Kind}' is incompatible with type '{targetType.Id}'.");
        }
    }
    private static GuestRegister? ResolveResult(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        IReadOnlyDictionary<string, GuestRegister> values,
        IReadOnlySet<string> localIds,
        HashSet<string> assignedValues)
    {
        if (instruction.ResultId is null)
        {
            return null;
        }

        if (!values.TryGetValue(instruction.ResultId, out GuestRegister? result))
        {
            context.Add(
                "ASIR1004",
                $"Function '{function.Id}' instruction result references unknown value '{instruction.ResultId}'.");
            return null;
        }

        if (!localIds.Contains(result.Id) || !assignedValues.Add(result.Id))
        {
            context.Add(
                "ASIR1008",
                $"Function '{function.Id}' instruction result '{result.Id}' is not a unique local definition.");
        }

        return result;
    }

    private static GuestRegister?[] ResolveOperands(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        IReadOnlyDictionary<string, GuestRegister> values)
    {
        GuestRegister?[] operands = new GuestRegister?[instruction.OperandIds.Count];
        for (int index = 0; index < instruction.OperandIds.Count; ++index)
        {
            string operandId = instruction.OperandIds[index];
            if (!values.TryGetValue(operandId, out GuestRegister? operand))
            {
                context.Add(
                    "ASIR1004",
                    $"Function '{function.Id}' instruction references unknown value '{operandId}'.");
            }

            operands[index] = operand;
        }

        return operands;
    }

    private static void ValidateConstantInstruction(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result)
    {
        if (result is null
            || instruction.Constant is null
            || instruction.OperandIds.Count != 0
            || instruction.TargetId is not null
            || instruction.OperatorKind is not null)
        {
            AddMalformedInstruction(context, function, instruction);
            return;
        }

        if (context.Types.TryGetValue(result.TypeId, out GuestType? targetType))
        {
            ValidateConstant(
                context,
                instruction.Constant,
                targetType,
                $"Function '{function.Id}' result '{result.Id}'");
        }
    }

    private static void ValidateCopy(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        if (result is null || operands.Count != 1 || instruction.TargetId is not null
            || instruction.OperatorKind is not null || instruction.Constant is not null)
        {
            AddMalformedInstruction(context, function, instruction);
            return;
        }

        if (operands[0] is not null
            && !string.Equals(result.TypeId, operands[0]!.TypeId, StringComparison.Ordinal))
        {
            AddTypeMismatch(context, function, instruction);
        }
    }

    private static void ValidateBinary(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        if (result is null || operands.Count != 2 || string.IsNullOrWhiteSpace(instruction.OperatorKind)
            || instruction.TargetId is not null || instruction.Constant is not null)
        {
            AddMalformedInstruction(context, function, instruction);
            return;
        }

        if (operands[0] is null || operands[1] is null)
        {
            return;
        }

        if (!string.Equals(operands[0]!.TypeId, operands[1]!.TypeId, StringComparison.Ordinal))
        {
            AddTypeMismatch(context, function, instruction);
            return;
        }

        bool comparison = instruction.OperatorKind is "equals" or "not_equals"
            or "less_than" or "less_than_or_equal"
            or "greater_than" or "greater_than_or_equal";
        if (comparison)
        {
            if (!context.Types.TryGetValue(result.TypeId, out GuestType? resultType)
                || !string.Equals(resultType.Storage, "i32", StringComparison.Ordinal))
            {
                AddTypeMismatch(context, function, instruction);
            }
        }
        else if (!string.Equals(result.TypeId, operands[0]!.TypeId, StringComparison.Ordinal))
        {
            AddTypeMismatch(context, function, instruction);
        }
    }

    private static void ValidateConvert(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        if (result is null || operands.Count != 1 || instruction.TargetId is not null
            || instruction.Constant is not null)
        {
            AddMalformedInstruction(context, function, instruction);
            return;
        }

        if (operands[0] is null
            || !context.Types.TryGetValue(operands[0]!.TypeId, out GuestType? sourceType)
            || !context.Types.TryGetValue(result.TypeId, out GuestType? targetType))
        {
            return;
        }

        if (string.Equals(instruction.OperatorKind, "class_ref_ordinal", StringComparison.Ordinal))
        {
            if (sourceType.Kind != "class_ref"
                || targetType.Kind != "scalar"
                || targetType.Storage != "i32"
                || !string.Equals(result.TypeId, "type:int32", StringComparison.Ordinal))
            {
                AddTypeMismatch(context, function, instruction);
            }
            return;
        }

        if (instruction.OperatorKind is not null
            || sourceType.Kind == "class_ref"
            || targetType.Kind == "class_ref")
        {
            AddTypeMismatch(context, function, instruction);
        }
    }

    private static void ValidateCall(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction,
        GuestRegister? result,
        IReadOnlyList<GuestRegister?> operands)
    {
        if (instruction.TargetId is null || instruction.OperatorKind is not null || instruction.Constant is not null)
        {
            context.Add("ASIR1009", $"Function '{function.Id}' has a malformed call instruction.");
            return;
        }

        IReadOnlyList<string>? parameterTypeIds = null;
        string? returnTypeId = null;
        if (context.Imports.TryGetValue(instruction.TargetId, out GuestImport? import))
        {
            parameterTypeIds = import.ParameterTypeIds;
            returnTypeId = import.ReturnTypeId;
        }
        else if (context.Functions.TryGetValue(instruction.TargetId, out GuestFunction? targetFunction))
        {
            string[] functionParameterTypes = new string[targetFunction.Parameters.Count];
            for (int index = 0; index < targetFunction.Parameters.Count; ++index)
            {
                functionParameterTypes[index] = targetFunction.Parameters[index].TypeId;
            }

            parameterTypeIds = functionParameterTypes;
            returnTypeId = targetFunction.ReturnTypeId;
        }

        if (parameterTypeIds is null || returnTypeId is null || operands.Count != parameterTypeIds.Count)
        {
            AddCallMismatch(context, function, instruction);
            return;
        }

        for (int index = 0; index < operands.Count; ++index)
        {
            if (operands[index] is not null
                && !string.Equals(operands[index]!.TypeId, parameterTypeIds[index], StringComparison.Ordinal))
            {
                AddCallMismatch(context, function, instruction);
                return;
            }
        }

        bool returnsVoid = context.IsVoidType(returnTypeId);
        if ((returnsVoid && instruction.ResultId is not null)
            || (!returnsVoid
                && (result is null || !string.Equals(result.TypeId, returnTypeId, StringComparison.Ordinal))))
        {
            AddCallMismatch(context, function, instruction);
        }
    }

    private static void AddMalformedInstruction(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction)
    {
        context.Add(
            "ASIR1008",
            $"Function '{function.Id}' has malformed '{instruction.Op}' instruction operands or metadata.");
    }

    private static void AddTypeMismatch(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction)
    {
        context.Add(
            "ASIR1008",
            $"Function '{function.Id}' instruction '{instruction.Op}' has incompatible value types.");
    }

    private static void AddCallMismatch(
        GuestValidationContext context,
        GuestFunction function,
        GuestInstruction instruction)
    {
        context.Add(
            "ASIR1009",
            $"Function '{function.Id}' call target '{instruction.TargetId}' has an incompatible signature.");
    }
}

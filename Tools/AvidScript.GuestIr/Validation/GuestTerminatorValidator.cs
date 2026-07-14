using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestTerminatorValidator
{
    public static void Validate(
        GuestValidationContext context,
        GuestFunction function,
        GuestTerminator terminator,
        IReadOnlyDictionary<string, GuestRegister> values,
        IReadOnlyDictionary<string, GuestBasicBlock> blocks)
    {
        switch (terminator.Kind)
        {
            case "branch":
                if (terminator.TargetBlockId is null
                    || terminator.ConditionValueId is not null
                    || terminator.FalseTargetBlockId is not null
                    || terminator.ReturnValueId is not null)
                {
                    AddMalformedTerminator(context, function, terminator);
                }
                else
                {
                    RequireBlock(context, function, blocks, terminator.TargetBlockId);
                }

                break;
            case "branch_if":
                if (terminator.ConditionValueId is null
                    || terminator.TargetBlockId is null
                    || terminator.FalseTargetBlockId is null
                    || terminator.ReturnValueId is not null)
                {
                    AddMalformedTerminator(context, function, terminator);
                    break;
                }

                if (!values.TryGetValue(terminator.ConditionValueId, out GuestRegister? condition))
                {
                    context.Add(
                        "ASIR1004",
                        $"Function '{function.Id}' branch condition references unknown value '{terminator.ConditionValueId}'.");
                }
                else if (!context.Types.TryGetValue(condition.TypeId, out GuestType? conditionType)
                    || !string.Equals(conditionType.Storage, "i32", StringComparison.Ordinal))
                {
                    context.Add(
                        "ASIR1008",
                        $"Function '{function.Id}' branch condition '{condition.Id}' is not i32-compatible.");
                }

                RequireBlock(context, function, blocks, terminator.TargetBlockId);
                RequireBlock(context, function, blocks, terminator.FalseTargetBlockId);
                break;
            case "return":
                if (terminator.ConditionValueId is not null
                    || terminator.TargetBlockId is not null
                    || terminator.FalseTargetBlockId is not null)
                {
                    AddMalformedTerminator(context, function, terminator);
                    break;
                }

                ValidateReturn(context, function, terminator.ReturnValueId, values);
                break;
            case "trap":
                if (terminator.ConditionValueId is not null
                    || terminator.TargetBlockId is not null
                    || terminator.FalseTargetBlockId is not null
                    || terminator.ReturnValueId is not null)
                {
                    AddMalformedTerminator(context, function, terminator);
                }

                break;
            default:
                AddMalformedTerminator(context, function, terminator);
                break;
        }
    }

    private static void ValidateReturn(
        GuestValidationContext context,
        GuestFunction function,
        string? returnValueId,
        IReadOnlyDictionary<string, GuestRegister> values)
    {
        bool returnsVoid = context.IsVoidType(function.ReturnTypeId);
        if (returnsVoid)
        {
            if (returnValueId is not null)
            {
                context.Add("ASIR1008", $"Void function '{function.Id}' returns a value.");
            }

            return;
        }

        if (returnValueId is null)
        {
            context.Add("ASIR1008", $"Function '{function.Id}' does not return a value.");
            return;
        }

        if (!values.TryGetValue(returnValueId, out GuestRegister? returnValue))
        {
            context.Add(
                "ASIR1004",
                $"Function '{function.Id}' return references unknown value '{returnValueId}'.");
        }
        else if (!string.Equals(returnValue.TypeId, function.ReturnTypeId, StringComparison.Ordinal))
        {
            context.Add(
                "ASIR1008",
                $"Function '{function.Id}' return value '{returnValueId}' has the wrong type.");
        }
    }

    private static void RequireBlock(
        GuestValidationContext context,
        GuestFunction function,
        IReadOnlyDictionary<string, GuestBasicBlock> blocks,
        string blockId)
    {
        if (!blocks.ContainsKey(blockId))
        {
            context.Add(
                "ASIR1005",
                $"Function '{function.Id}' references unknown block '{blockId}'.");
        }
    }

    private static void AddMalformedTerminator(
        GuestValidationContext context,
        GuestFunction function,
        GuestTerminator terminator)
    {
        context.Add(
            "ASIR1006",
            $"Function '{function.Id}' has malformed terminator '{terminator.Kind}'.");
    }
}

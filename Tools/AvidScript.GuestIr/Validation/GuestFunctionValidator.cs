using System;
using System.Collections.Generic;

namespace AvidScript.GuestIr;

internal static class GuestFunctionValidator
{
    public static void Validate(GuestValidationContext context, GuestFunction function)
    {
        context.RequireType(function.ReturnTypeId, $"Function '{function.Id}' return value");

        Dictionary<string, GuestRegister> values = new(StringComparer.Ordinal);
        HashSet<string> localIds = new(StringComparer.Ordinal);
        IndexRegisters(context, function, function.Parameters, values, null, "parameter");
        IndexRegisters(context, function, function.Locals, values, localIds, "local");

        Dictionary<string, GuestBasicBlock> blocks = new(StringComparer.Ordinal);
        foreach (GuestBasicBlock block in function.Blocks)
        {
            if (string.IsNullOrWhiteSpace(block.Id) || !blocks.TryAdd(block.Id, block))
            {
                context.Add(
                    "ASIR1002",
                    $"Function '{function.Id}' has an empty or duplicated block id '{block.Id}'.");
            }
        }

        if (!blocks.ContainsKey(function.EntryBlockId))
        {
            context.Add(
                "ASIR1005",
                $"Function '{function.Id}' references unknown entry block '{function.EntryBlockId}'.");
        }

        HashSet<string> assignedValues = new(StringComparer.Ordinal);
        foreach (GuestBasicBlock block in function.Blocks)
        {
            foreach (GuestInstruction instruction in block.Instructions)
            {
                GuestInstructionValidator.Validate(
                    context,
                    function,
                    instruction,
                    values,
                    localIds,
                    assignedValues);
            }

            GuestTerminatorValidator.Validate(context, function, block.Terminator, values, blocks);
        }
    }

    private static void IndexRegisters(
        GuestValidationContext context,
        GuestFunction function,
        IReadOnlyList<GuestRegister> registers,
        Dictionary<string, GuestRegister> values,
        HashSet<string>? localIds,
        string kind)
    {
        foreach (GuestRegister register in registers)
        {
            if (string.IsNullOrWhiteSpace(register.Id) || !values.TryAdd(register.Id, register))
            {
                context.Add(
                    "ASIR1002",
                    $"Function '{function.Id}' has an empty or duplicated {kind} id '{register.Id}'.");
            }

            localIds?.Add(register.Id);
            context.RequireType(register.TypeId, $"Function '{function.Id}' {kind} '{register.Id}'");
        }
    }
}
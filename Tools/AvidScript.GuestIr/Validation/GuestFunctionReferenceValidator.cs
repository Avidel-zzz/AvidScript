using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestFunctionReferenceValidator
{
    public static void ValidateContracts(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        if (module.SchemaVersion < 3 && (module.FunctionReferences.Count != 0
            || module.Types.Any(type => type.Kind == "function_ref")))
            context.Add("ASIR1012", "Legacy Guest IR cannot contain function reference contracts.");
        HashSet<string> seen = new(StringComparer.Ordinal);
        foreach (GuestFunctionReference reference in module.FunctionReferences)
        {
            if (!seen.Add(reference.TypeId)
                || !context.Types.TryGetValue(reference.TypeId, out GuestType? type) || type.Kind != "function_ref"
                || !context.Types.ContainsKey(reference.ReturnTypeId)
                || reference.ParameterTypeIds.Any(id => !context.Types.ContainsKey(id) || context.IsVoidType(id))
                || reference.TargetFunctionIds.Distinct(StringComparer.Ordinal).Count() != reference.TargetFunctionIds.Count)
            {
                context.Add("ASIR1012", $"Function reference '{reference.TypeId}' has an invalid contract.");
                continue;
            }
            foreach (string id in reference.TargetFunctionIds)
            {
                if (!context.Functions.TryGetValue(id, out GuestFunction? target)
                    || target.ReturnTypeId != reference.ReturnTypeId
                    || !target.Parameters.Select(parameter => parameter.TypeId).SequenceEqual(reference.ParameterTypeIds))
                    context.Add("ASIR1012", $"Function reference '{reference.TypeId}' target '{id}' has an incompatible signature.");
            }
        }
        foreach (GuestType type in module.Types.Where(type => type.Kind == "function_ref"))
            if (!seen.Contains(type.Id)) context.Add("ASIR1012", $"Function reference '{type.Id}' has no signature contract.");
    }

    public static void ValidateInstruction(GuestValidationContext context, GuestFunction function,
        GuestInstruction instruction, GuestRegister? result, IReadOnlyList<GuestRegister?> operands)
    {
        bool creation = instruction.Op == "function_ref";
        string? typeId = creation ? result?.TypeId : instruction.TargetId;
        GuestFunctionReference? reference = context.Module.FunctionReferences.FirstOrDefault(item => item.TypeId == typeId);
        bool valid = context.Module.SchemaVersion >= 3 && reference is not null
            && instruction.Constant is null && instruction.OperatorKind is null;
        if (valid && creation)
            valid = operands.Count == 0 && instruction.TargetId is not null
                && reference!.TargetFunctionIds.Contains(instruction.TargetId, StringComparer.Ordinal);
        else if (valid)
            valid = operands.Count == reference!.ParameterTypeIds.Count + 1
                && operands[0]?.TypeId == reference.TypeId
                && operands.Skip(1).Select(operand => operand?.TypeId).SequenceEqual(reference.ParameterTypeIds)
                && (context.IsVoidType(reference.ReturnTypeId) ? result is null : result?.TypeId == reference.ReturnTypeId);
        if (!valid) context.Add("ASIR1012", $"Function '{function.Id}' has an invalid '{instruction.Op}' signature or target.");
    }
}

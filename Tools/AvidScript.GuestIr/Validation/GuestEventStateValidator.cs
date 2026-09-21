using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestEventStateValidator
{
    public static void Validate(GuestValidationContext context, GuestFunction function,
        GuestInstruction instruction, GuestRegister? result, IReadOnlyList<GuestRegister?> operands)
    {
        bool subscribe = instruction.Op == GuestEventState.SubscribeOp;
        bool Scalar(string? id, string storage, int size) => id is not null
            && context.Types.TryGetValue(id, out GuestType? type)
            && type.Kind == "scalar" && type.Storage == storage && type.Size == size;
        bool Concrete(string? id) => id is not null && context.Types.TryGetValue(id, out GuestType? type)
            && type.Kind == "managed_ref" && type.ElementTypeId is not null;
        bool valid = context.Module.SchemaVersion >= GuestEventState.MinimumSchemaVersion
            && instruction.Constant is null && instruction.OperatorKind is null
            && (subscribe
                ? operands.Count == 4 && operands.Take(3).All(value => Scalar(value?.TypeId, "i32", 4))
                    && Concrete(operands[3]?.TypeId) && Scalar(result?.TypeId, "i64", 8)
                : operands.Count == 0 && Concrete(result?.TypeId));
        if (instruction.TargetId is null || !context.Imports.TryGetValue(instruction.TargetId, out GuestImport? import))
            valid = false;
        else
            valid &= import.Module == GuestEventState.ImportModule
                && import.Name == (subscribe ? GuestEventState.SubscribeImport : GuestEventState.ReadImport)
                && import.OptimizationClass == "none" && import.BindingOrdinal == -1
                && Scalar(import.ReturnTypeId, "i64", 8)
                && (subscribe
                    ? import.ParameterTypeIds.Count == 5 && import.ParameterTypeIds.Take(4).All(id => Scalar(id, "i32", 4))
                        && Scalar(import.ParameterTypeIds[4], "i64", 8)
                    : import.ParameterTypeIds.Count == 1 && Scalar(import.ParameterTypeIds[0], "i32", 4));
        if (!valid) context.Add("ASIR1013", $"Function '{function.Id}' has invalid '{instruction.Op}': expected IR 12/1.11, a concrete state reference and the declared event-state ABI.");
    }
}

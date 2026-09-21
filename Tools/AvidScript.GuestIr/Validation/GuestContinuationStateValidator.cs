using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestContinuationStateValidator
{
    public static void Validate(GuestValidationContext context, GuestFunction function,
        GuestInstruction instruction, GuestRegister? result, IReadOnlyList<GuestRegister?> operands)
    {
        bool store = instruction.Op == GuestContinuationState.StoreOp;
        bool Scalar(string? id, string storage, int size) => id is not null
            && context.Types.TryGetValue(id, out GuestType? type)
            && type.Kind == "scalar" && type.Storage == storage && type.Size == size;
        bool Concrete(string? id) => id is not null && context.Types.TryGetValue(id, out GuestType? type)
            && type.Kind == "managed_ref" && type.ElementTypeId is not null;
        bool valid = context.Module.SchemaVersion >= GuestContinuationState.MinimumSchemaVersion
            && instruction.Constant is null && instruction.OperatorKind is null
            && operands.Count == (store ? 2 : 1) && Scalar(operands.FirstOrDefault()?.TypeId, "i64", 8)
            && (store ? Scalar(result?.TypeId, "i32", 4) && Concrete(operands.LastOrDefault()?.TypeId)
                : Concrete(result?.TypeId));

        // The import must be declared and must be the precise native contract, not
        // a same-signature arbitrary host function or an optimized binding route.
        if (instruction.TargetId is null || !context.Imports.TryGetValue(instruction.TargetId, out GuestImport? import))
            valid = false;
        else
        {
            valid &= import.Module == GuestContinuationState.ImportModule
                && import.Name == (store ? GuestContinuationState.StoreImport : GuestContinuationState.ReadImport)
                && import.OptimizationClass == "none" && import.BindingOrdinal == -1
                && import.ParameterTypeIds.Count == (store ? 3 : 2)
                && Scalar(import.ParameterTypeIds.ElementAtOrDefault(0), "i64", 8)
                && Scalar(import.ParameterTypeIds.ElementAtOrDefault(1), "i32", 4)
                && (store ? Scalar(import.ParameterTypeIds.ElementAtOrDefault(2), "i64", 8) && Scalar(import.ReturnTypeId, "i32", 4)
                    : Scalar(import.ReturnTypeId, "i64", 8));
        }
        if (!valid) context.Add("ASIR1013", $"Function '{function.Id}' has invalid '{instruction.Op}': expected IR 11/1.10, a concrete managed reference, an i64 continuation token and the declared state ABI.");
    }
}

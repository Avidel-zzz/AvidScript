using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestFramedCallValidator
{
    public static void ValidateContracts(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        if ((module.SchemaVersion < 7 && module.FramedExports.Count != 0)
            || module.FramedExports.Count > GuestCallFrameLayout.MaxExports)
            Add(context, "Framed exports require IR 7/1.6 and a bounded export count.");
        HashSet<string> names = new(module.Exports.Select(export => export.Name), StringComparer.Ordinal) { "memory" };
        foreach (GuestFramedExport export in module.FramedExports)
        {
            if (string.IsNullOrWhiteSpace(export.Name) || !names.Add(export.Name)
                || !context.Functions.TryGetValue(export.FunctionId, out GuestFunction? function)
                || export.ParameterKinds.Count != function.Parameters.Count
                || function.Parameters.Count > GuestCallFrameLayout.MaxParameters
                || !context.Types.TryGetValue(function.ReturnTypeId, out GuestType? result)
                || result.Kind == GuestBorrowedReference.Kind)
            { Add(context, $"Framed export '{export.Name}' has an invalid name, body or signature."); continue; }
            bool valid = true;
            for (int i = 0; i < function.Parameters.Count; ++i)
            {
                string kind = export.ParameterKinds[i];
                if (kind is not ("value" or "ref" or "out" or "in")
                    || !context.Types.TryGetValue(function.Parameters[i].TypeId, out GuestType? type)
                    || type.Storage == "none" || (kind != "value") != (type.Kind == GuestBorrowedReference.Kind))
                    valid = false;
            }
            if (!valid) { Add(context, $"Framed export '{export.Name}' has incompatible parameter passing kinds."); continue; }
            try { GuestCallFrameLayout.Create(export, function, context.Types, module.FunctionReferences); }
            catch (Exception exception) when (exception is InvalidOperationException or KeyNotFoundException or OverflowException)
            { Add(context, $"Framed export '{export.Name}': {exception.Message}"); }
        }
    }

    public static void ValidateInstruction(GuestValidationContext context, GuestFunction function,
        GuestInstruction instruction, GuestRegister? result, IReadOnlyList<GuestRegister?> operands)
    {
        GuestFramedExport? export = context.Module.FramedExports.FirstOrDefault(item => item.Name == instruction.TargetId);
        bool valid = context.Module.SchemaVersion >= 7 && export is not null
            && instruction.Constant is null && instruction.OperatorKind is null
            && context.Functions.TryGetValue(export.FunctionId, out _);
        if (valid)
        {
            GuestFunction target = context.Functions[export!.FunctionId];
            valid = target.Parameters.Select(parameter => parameter.TypeId).SequenceEqual(operands.Select(operand => operand?.TypeId))
                && (context.IsVoidType(target.ReturnTypeId) ? result is null : result?.TypeId == target.ReturnTypeId);
        }
        if (!valid) Add(context, $"Function '{function.Id}' has an invalid framed call signature or target.");
    }

    private static void Add(GuestValidationContext context, string message) => context.Add("ASIR1015", message);
}

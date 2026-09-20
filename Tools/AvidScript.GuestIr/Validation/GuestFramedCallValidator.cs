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
        Dictionary<string, GuestCallFrameLayout> layouts = new(StringComparer.Ordinal);
        foreach (GuestFramedExport export in module.FramedExports)
        {
            if (export.HostImportId is { } hostId)
            {
                GuestImport? host = module.Imports.FirstOrDefault(import => import.Id == hostId);
                bool I32(string typeId) => context.Types.TryGetValue(typeId, out GuestType? type)
                    && type.Storage == "i32" && type.Size == 4;
                if (module.SchemaVersion < 9 || string.IsNullOrWhiteSpace(hostId) || host is null
                    || host.ParameterTypeIds.Count != 2 || !host.ParameterTypeIds.All(I32) || !I32(host.ReturnTypeId))
                    Add(context, $"Framed export '{export.Name}' requires IR 9/1.8 and a synchronous (i32,i32)->i32 host route.");
            }
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
            try { layouts.TryAdd(export.Name, GuestCallFrameLayout.Create(export, function, context.Types, module.FunctionReferences)); }
            catch (Exception exception) when (exception is InvalidOperationException or KeyNotFoundException or OverflowException)
            { Add(context, $"Framed export '{export.Name}': {exception.Message}"); }
        }
        ValidateDispatchTargets(context, layouts);
    }

    private static void ValidateDispatchTargets(GuestValidationContext context,
        IReadOnlyDictionary<string, GuestCallFrameLayout> layouts)
    {
        var exports = context.Module.FramedExports.GroupBy(export => export.Name, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        long total = 0;
        foreach (GuestFramedExport route in context.Module.FramedExports)
        {
            if (route.HostDispatchTargets is not { } targets) continue;
            total += targets.Count;
            if (context.Module.SchemaVersion < 10 || route.HostImportId is null
                || targets.Count is 0 or > GuestCallFrameLayout.MaxExports || total > 65536)
            { Add(context, $"Framed export '{route.Name}' requires IR 10/1.9, a host import and bounded nonempty dispatch targets."); continue; }
            uint? previous = null;
            foreach (GuestHostDispatchTarget entry in targets)
            {
                if (previous is { } key && entry.Selector <= key)
                    Add(context, $"Framed export '{route.Name}' dispatch selectors must be unique and ascending.");
                previous = entry.Selector;
                if (!exports.TryGetValue(entry.ExportName, out GuestFramedExport? target)
                    || target.Name == route.Name || target.HostDispatchTargets is not null || target.HostImportId is null
                    || !layouts.TryGetValue(route.Name, out GuestCallFrameLayout? sourceLayout)
                    || !layouts.TryGetValue(target.Name, out GuestCallFrameLayout? targetLayout)
                    || sourceLayout.SignatureSha256 != targetLayout.SignatureSha256
                    || !context.Functions.TryGetValue(route.FunctionId, out GuestFunction? sourceFunction)
                    || !context.Functions.TryGetValue(target.FunctionId, out GuestFunction? targetFunction)
                    || sourceFunction.ReturnTypeId != targetFunction.ReturnTypeId
                    || !sourceFunction.Parameters.Select(parameter => parameter.TypeId)
                        .SequenceEqual(targetFunction.Parameters.Select(parameter => parameter.TypeId))
                    || !route.ParameterKinds.SequenceEqual(target.ParameterKinds))
                    Add(context, $"Framed export '{route.Name}' dispatch target '{entry.ExportName}' must be a direct host frame with the same nominal signature and layout.");
            }
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

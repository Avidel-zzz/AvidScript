using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestManagedHeapValidator
{
    public static void ValidateContracts(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        GuestType[] references = module.Types.Where(type => type.Kind == "managed_ref").ToArray();
        if (references.Length == 0) return;
        if (module.SchemaVersion < 4 || references.Length > GuestManagedHeap.MaxLayouts)
            Add(context, "Managed references require Guest IR 4/1.3 and a bounded layout table.");
        if (references.Any(type => type.ElementTypeId is null)
            && (module.SchemaVersion < 5 || module.SchemaVersion < 8 && references.All(type => type.ElementTypeId is null)))
            Add(context, "Erased managed references require Guest IR 5/1.4; roots-only configurations require IR 8/1.7.");
        GuestImport[] imports = module.Imports.Where(import => import.Module == GuestManagedHeap.ImportModule
            && import.Name == GuestManagedHeap.ImportName).ToArray();
        bool IsI32(string id) => context.Types.TryGetValue(id, out GuestType? type) && type.Kind == "scalar" && type.Storage == "i32" && type.Size == 4;
        if (imports.Length != 1 || imports[0].ParameterTypeIds.Count != 4 || !imports[0].ParameterTypeIds.All(IsI32)
            || !IsI32(imports[0].ReturnTypeId) || imports[0].OptimizationClass != "none")
            Add(context, "Managed references require exactly one declared managed heap v1 packet import with (iiii)i ABI.");
        int totalReferences = 0;
        foreach (GuestType reference in references)
        {
            if (reference.ElementTypeId is null) continue;
            if (!context.Types.TryGetValue(reference.ElementTypeId, out GuestType? payload)
                || payload.Kind != "struct" || payload.Size <= 0 || payload.Size > GuestManagedHeap.MaxObjectBytes)
            { Add(context, $"Managed reference '{reference.Id}' requires a nonempty bounded struct payload."); continue; }
            try
            {
                IReadOnlyList<GuestManagedLeaf> leaves = GuestManagedHeap.Leaves(context.Types, payload.Id);
                int count = leaves.Count(leaf => leaf.Type.Kind == "managed_ref");
                totalReferences = checked(totalReferences + count);
                if (count > GuestManagedHeap.MaxReferencesPerLayout || totalReferences > GuestManagedHeap.MaxTotalReferences
                    || leaves.Any(leaf => leaf.Type.Kind is not ("scalar" or "enum" or "function_ref" or "managed_ref" or "handle")))
                    Add(context, $"Managed payload '{payload.Id}' contains borrowed storage or exceeds reference limits.");
            }
            catch (Exception error) when (error is KeyNotFoundException or InvalidOperationException or OverflowException)
            { Add(context, $"Managed payload '{payload.Id}' has invalid or unbounded value layout."); }
        }
        bool Has(string id) => GuestManagedHeap.ContainsReferences(context.Types, id);
        if (module.Globals.Any(value => Has(value.TypeId)) || module.DataSegments.Any(value => Has(value.TypeId))
            || module.Types.Any(type => type.Kind == "array" && type.ElementTypeId is not null && Has(type.ElementTypeId)))
            Add(context, "Managed references cannot enter untraced globals, static data or array storage.");
        bool combinedVersion = module.SchemaVersion == GuestTaskLanguageErrorValidator.SchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.IrVersion
            || module.SchemaVersion == GuestTaskLanguageErrorValidator.AsyncSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.AsyncIrVersion
            || module.SchemaVersion == GuestTaskLanguageErrorValidator.ExceptionFlowSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.ExceptionFlowIrVersion
            || module.SchemaVersion == GuestTaskLanguageErrorValidator.DirectCleanupSchemaVersion
            && module.IrVersion == GuestTaskLanguageErrorValidator.DirectCleanupIrVersion;
        bool catalogVersion = module.SchemaVersion == GuestLanguageErrorCatalogValidator.SchemaVersion
            && module.IrVersion == GuestLanguageErrorCatalogValidator.IrVersion;
        foreach (GuestImport import in module.Imports)
        {
            bool report = (catalogVersion || combinedVersion)
                && module.LanguageErrorCatalog is not null
                && import.Id == "import:language_error_report_v1"
                && import.Module == "avidscript"
                && import.Name == "avid_language_error_report_v1"
                && import.ParameterTypeIds.SequenceEqual(new[]
                    { "type:int32", "type:int32", "type:language_error_root" })
                && import.ReturnTypeId == "type:int32"
                && import.OptimizationClass == "none";
            bool taskFault = combinedVersion && module.LanguageErrorCatalog is not null
                && import.Id == GuestTaskLanguageErrorValidator.ImportId
                && import.Module == GuestTaskResultValidator.ImportModule
                && import.Name == GuestTaskLanguageErrorValidator.ImportName
                && import.ParameterTypeIds.SequenceEqual(new[]
                    { "type:int64", "type:int32", "type:int32", "type:language_error_root" })
                && import.ReturnTypeId == "type:int32"
                && import.OptimizationClass == "none";
            bool taskReadRoot = combinedVersion && module.LanguageErrorCatalog is not null
                && import.Id == GuestTaskLanguageErrorValidator.RootImportId
                && import.Module == GuestTaskResultValidator.ImportModule
                && import.Name == GuestTaskLanguageErrorValidator.RootImportName
                && import.ParameterTypeIds.SequenceEqual(new[] { "type:int64" })
                && import.ReturnTypeId == "type:language_error_root"
                && import.DispatchClass == "semantic" && import.OptimizationClass == "none"
                && import.BindingOrdinal == -1;
            if ((Has(import.ReturnTypeId) || import.ParameterTypeIds.Any(Has))
                && !report && !taskFault && !taskReadRoot)
                Add(context, $"Import '{import.Id}' cannot expose module-local managed references.");
        }
        foreach (GuestExport export in module.Exports)
            if (context.Functions.TryGetValue(export.FunctionId, out GuestFunction? function)
                && (Has(function.ReturnTypeId) || function.Parameters.Any(parameter => Has(parameter.TypeId))))
                Add(context, $"Export '{export.Name}' requires a persistent-root adapter before exposing managed references.");
    }

    public static void ValidateInstruction(GuestValidationContext context, GuestFunction function,
        GuestInstruction instruction, GuestRegister? result, IReadOnlyList<GuestRegister?> operands,
        IReadOnlyDictionary<string, GuestRegister> values)
    {
        bool Has(string? id) => id is not null && GuestManagedHeap.ContainsReferences(context.Types, id);
        if (instruction.Op == "address_of" && instruction.TargetId is not null && values.TryGetValue(instruction.TargetId, out GuestRegister? target) && Has(target.TypeId)
            || instruction.Op is "indirect_load" or "indirect_store" or "convert" && (Has(result?.TypeId) || operands.Any(operand => Has(operand?.TypeId))))
            Add(context, $"Function '{function.Id}' cannot expose an untraced address alias of managed storage.");
        if (instruction.Op is GuestContinuationState.StoreOp or GuestContinuationState.ReadOp)
        {
            GuestContinuationStateValidator.Validate(context, function, instruction, result, operands);
            return;
        }
        if (instruction.Op is GuestEventState.SubscribeOp or GuestEventState.ReadOp
            or GuestEventState.LanguageSubscribeOp or GuestEventState.LanguageLookupOp)
        {
            GuestEventStateValidator.Validate(context, function, instruction, result, operands);
            return;
        }
        if (!instruction.Op.StartsWith("managed_", StringComparison.Ordinal)) return;
        bool valid = context.Module.SchemaVersion >= 4 && context.Module.Types.Any(type => type.Kind == "managed_ref")
            && instruction.Constant is null && instruction.OperatorKind is null;
        if (instruction.Op == "managed_new")
            valid &= result is not null && context.Types.TryGetValue(result.TypeId, out GuestType? allocated)
                && allocated.Kind == "managed_ref" && allocated.ElementTypeId is not null && operands.Count == 0 && instruction.TargetId is null;
        else if (instruction.Op == "managed_cast")
            valid &= context.Module.SchemaVersion >= 5 && result is not null
                && context.Types.TryGetValue(result.TypeId, out GuestType? castType) && castType.Kind == "managed_ref"
                && operands.Count == 1 && operands[0] is { } source
                && context.Types.TryGetValue(source.TypeId, out GuestType? sourceType) && sourceType.Kind == "managed_ref"
                && instruction.TargetId is null;
        else if (instruction.Op == "managed_collect")
            valid &= result is null && operands.Count == 0 && instruction.TargetId is null;
        else if (instruction.Op is "managed_get" or "managed_set")
        {
            GuestField? field = operands.Count > 0 && operands[0] is { } owner
                && context.Types.TryGetValue(owner.TypeId, out GuestType? reference) && reference.Kind == "managed_ref"
                && reference.ElementTypeId is not null && context.Types.TryGetValue(reference.ElementTypeId, out GuestType? payload)
                ? payload.Fields.FirstOrDefault(item => item.Id == instruction.TargetId) : null;
            valid &= field is not null && (instruction.Op == "managed_get"
                ? operands.Count == 1 && result?.TypeId == field.TypeId
                : result is null && operands.Count == 2 && operands[1]?.TypeId == field.TypeId);
        }
        else valid = false;
        if (!valid) Add(context, $"Function '{function.Id}' has invalid '{instruction.Op}' operands, field or version.");
    }
    private static void Add(GuestValidationContext context, string message) => context.Add("ASIR1013", message);
}

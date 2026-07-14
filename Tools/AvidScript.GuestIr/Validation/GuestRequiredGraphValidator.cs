using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestRequiredGraphValidator
{
    public static bool IsValid(GuestModule module)
    {
        if (module.Provenance is null
            || module.Types is null
            || module.Imports is null
            || module.Globals is null
            || module.Functions is null
            || module.Exports is null
            || module.Diagnostics is null
            || HasNull(module.ModuleId, module.Language))
        {
            return false;
        }

        GuestProvenance provenance = module.Provenance;
        if (HasNull(
            provenance.SourceId,
            provenance.SourceSha256,
            provenance.FrontendSha256,
            provenance.SemanticSha256,
            provenance.SemanticVersion))
        {
            return false;
        }

        foreach (GuestType? type in module.Types)
        {
            if (type?.Fields is null || HasNull(type.Id, type.Kind, type.Storage))
            {
                return false;
            }

            foreach (GuestField? field in type.Fields)
            {
                if (field is null || HasNull(field.Id, field.Name, field.TypeId))
                {
                    return false;
                }
            }
        }

        foreach (GuestImport? import in module.Imports)
        {
            if (import?.ParameterTypeIds is null
                || HasNull(import.Id, import.Module, import.Name, import.ReturnTypeId)
                || import.ParameterTypeIds.Any(typeId => typeId is null))
            {
                return false;
            }
        }

        foreach (GuestGlobal? global in module.Globals)
        {
            if (global?.InitialValue is null
                || HasNull(global.Id, global.TypeId, global.InitialValue.Kind))
            {
                return false;
            }
        }

        foreach (GuestFunction? function in module.Functions)
        {
            if (function?.Parameters is null
                || function.Locals is null
                || function.Blocks is null
                || HasNull(function.Id, function.ReturnTypeId, function.EntryBlockId))
            {
                return false;
            }

            foreach (GuestRegister? register in function.Parameters.Concat(function.Locals))
            {
                if (register is null || HasNull(register.Id, register.TypeId))
                {
                    return false;
                }
            }

            foreach (GuestBasicBlock? block in function.Blocks)
            {
                if (block?.Instructions is null
                    || block.Terminator is null
                    || HasNull(block.Id, block.Terminator.Kind))
                {
                    return false;
                }

                foreach (GuestInstruction? instruction in block.Instructions)
                {
                    if (instruction?.OperandIds is null
                        || HasNull(instruction.Op)
                        || instruction.OperandIds.Any(operandId => operandId is null)
                        || (instruction.Constant is not null && HasNull(instruction.Constant.Kind)))
                    {
                        return false;
                    }
                }
            }
        }

        foreach (GuestExport? export in module.Exports)
        {
            if (export is null || HasNull(export.Name, export.FunctionId))
            {
                return false;
            }
        }

        foreach (GuestDiagnostic? diagnostic in module.Diagnostics)
        {
            if (diagnostic is null || HasNull(diagnostic.Code, diagnostic.Severity, diagnostic.Message))
            {
                return false;
            }
        }

        return true;
    }

    private static bool HasNull(params string?[] values)
    {
        return values.Any(value => value is null);
    }
}

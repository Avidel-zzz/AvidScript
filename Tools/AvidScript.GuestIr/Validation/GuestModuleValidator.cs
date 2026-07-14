using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

public static class GuestModuleValidator
{
    public const int CurrentSchemaVersion = 1;
    public const string CurrentIrVersion = "1.0";

    public static GuestValidationResult Validate(GuestModule module)
    {
        ArgumentNullException.ThrowIfNull(module);

        if (!GuestRequiredGraphValidator.IsValid(module))
        {
            GuestDiagnostic diagnostic = new(
                "ASIR1001",
                "error",
                "Guest IR contains a null required object or collection.",
                null);
            return new GuestValidationResult(false, new[] { diagnostic });
        }

        GuestValidationContext context = new(module);
        ValidateHeader(context);
        IndexTopLevelIds(context);
        ValidateTypes(context);
        ValidateImports(context);
        ValidateGlobals(context);
        ValidateFunctions(context);
        ValidateExports(context);
        ValidateReportedStatus(context);

        GuestDiagnostic[] diagnostics = context.Diagnostics
            .OrderBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Message, StringComparer.Ordinal)
            .ToArray();
        bool succeeded = diagnostics.All(
            diagnostic => !string.Equals(diagnostic.Severity, "error", StringComparison.OrdinalIgnoreCase));
        return new GuestValidationResult(succeeded, diagnostics);
    }

    private static void ValidateHeader(GuestValidationContext context)
    {
        GuestModule module = context.Module;
        GuestProvenance provenance = module.Provenance;
        if (module.SchemaVersion != CurrentSchemaVersion
            || !string.Equals(module.IrVersion, CurrentIrVersion, StringComparison.Ordinal)
            || string.IsNullOrWhiteSpace(module.ModuleId)
            || string.IsNullOrWhiteSpace(module.Language)
            || string.IsNullOrWhiteSpace(provenance.SourceId)
            || !IsSha256(provenance.SourceSha256)
            || !IsSha256(provenance.FrontendSha256)
            || !IsSha256(provenance.SemanticSha256)
            || provenance.SemanticSchemaVersion <= 0
            || string.IsNullOrWhiteSpace(provenance.SemanticVersion))
        {
            context.Add("ASIR1001", "Guest IR header or provenance is invalid.");
        }
    }

    private static void IndexTopLevelIds(GuestValidationContext context)
    {
        HashSet<string> ids = new(StringComparer.Ordinal);
        foreach (GuestType type in context.Module.Types)
        {
            AddTopLevelId(context, ids, type.Id, "type");
            context.Types.TryAdd(type.Id, type);
        }

        foreach (GuestImport import in context.Module.Imports)
        {
            AddTopLevelId(context, ids, import.Id, "import");
            context.Imports.TryAdd(import.Id, import);
        }

        foreach (GuestGlobal global in context.Module.Globals)
        {
            AddTopLevelId(context, ids, global.Id, "global");
        }

        foreach (GuestFunction function in context.Module.Functions)
        {
            AddTopLevelId(context, ids, function.Id, "function");
            context.Functions.TryAdd(function.Id, function);
        }
    }

    private static void AddTopLevelId(
        GuestValidationContext context,
        HashSet<string> ids,
        string id,
        string kind)
    {
        if (string.IsNullOrWhiteSpace(id) || !ids.Add(id))
        {
            context.Add("ASIR1002", $"Guest IR {kind} id '{id}' is empty or duplicated.");
        }
    }

    private static void ValidateTypes(GuestValidationContext context)
    {
        foreach (GuestType type in context.Module.Types)
        {
            if (string.IsNullOrWhiteSpace(type.Kind)
                || string.IsNullOrWhiteSpace(type.Storage)
                || type.Size < 0
                || type.Alignment <= 0
                || (type.Alignment & (type.Alignment - 1)) != 0)
            {
                context.Add("ASIR1008", $"Type '{type.Id}' has an invalid shape or layout.");
            }

            HashSet<string> fieldIds = new(StringComparer.Ordinal);
            foreach (GuestField field in type.Fields)
            {
                if (string.IsNullOrWhiteSpace(field.Id) || !fieldIds.Add(field.Id))
                {
                    context.Add("ASIR1002", $"Type '{type.Id}' has an empty or duplicated field id '{field.Id}'.");
                }

                context.RequireType(field.TypeId, $"Field '{field.Id}'");
                if (field.Offset < 0)
                {
                    context.Add("ASIR1008", $"Field '{field.Id}' has a negative offset.");
                }
            }

            if (type.ElementTypeId is not null)
            {
                context.RequireType(type.ElementTypeId, $"Type '{type.Id}'");
            }

            if (type.UnderlyingTypeId is not null)
            {
                context.RequireType(type.UnderlyingTypeId, $"Type '{type.Id}'");
            }
        }
    }

    private static void ValidateImports(GuestValidationContext context)
    {
        foreach (GuestImport import in context.Module.Imports)
        {
            if (string.IsNullOrWhiteSpace(import.Module) || string.IsNullOrWhiteSpace(import.Name))
            {
                context.Add("ASIR1009", $"Import '{import.Id}' has an invalid host module or name.");
            }

            for (int index = 0; index < import.ParameterTypeIds.Count; ++index)
            {
                context.RequireType(import.ParameterTypeIds[index], $"Import '{import.Id}' parameter {index}");
            }

            context.RequireType(import.ReturnTypeId, $"Import '{import.Id}' return value");
        }
    }

    private static void ValidateGlobals(GuestValidationContext context)
    {
        foreach (GuestGlobal global in context.Module.Globals)
        {
            if (context.RequireType(global.TypeId, $"Global '{global.Id}'"))
            {
                GuestInstructionValidator.ValidateConstant(
                    context,
                    global.InitialValue,
                    context.Types[global.TypeId],
                    $"Global '{global.Id}' initializer");
            }
        }
    }

    private static void ValidateFunctions(GuestValidationContext context)
    {
        foreach (GuestFunction function in context.Module.Functions)
        {
            GuestFunctionValidator.Validate(context, function);
        }
    }

    private static void ValidateExports(GuestValidationContext context)
    {
        HashSet<string> names = new(StringComparer.Ordinal);
        foreach (GuestExport export in context.Module.Exports)
        {
            if (string.IsNullOrWhiteSpace(export.Name)
                || !names.Add(export.Name)
                || !context.Functions.ContainsKey(export.FunctionId))
            {
                context.Add(
                    "ASIR1007",
                    $"Export '{export.Name}' is duplicated, empty, or targets unknown function '{export.FunctionId}'.");
            }
        }
    }

    private static void ValidateReportedStatus(GuestValidationContext context)
    {
        bool reportedError = context.Module.Diagnostics.Any(
            diagnostic => string.Equals(diagnostic.Severity, "error", StringComparison.OrdinalIgnoreCase));
        if (context.Module.Succeeded == reportedError)
        {
            context.Add(
                "ASIR1010",
                "Guest IR succeeded flag is inconsistent with its reported diagnostics.");
        }
    }

    private static bool IsSha256(string value)
    {
        if (value.Length != 64)
        {
            return false;
        }

        foreach (char character in value)
        {
            if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
            {
                return false;
            }
        }

        return true;
    }
}

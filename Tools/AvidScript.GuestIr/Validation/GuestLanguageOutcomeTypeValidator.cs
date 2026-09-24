using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestLanguageOutcomeTypeValidator
{
    public const int SchemaVersion = 15;
    public const string IrVersion = "1.14";

    public static void Validate(GuestValidationContext context)
    {
        IReadOnlyList<GuestLanguageOutcomeType>? declarations = context.Module.LanguageOutcomeTypes;
        if (declarations is null)
        {
            if (context.Module.SchemaVersion is SchemaVersion or GuestLanguageOutcomeFlowValidator.SchemaVersion
                or GuestLanguageErrorCatalogValidator.SchemaVersion)
                context.Add("ASIR1024", "IR 15 through 17 require an explicit language outcome type list.");
            return;
        }

        bool validVersion = (context.Module.SchemaVersion == SchemaVersion && context.Module.IrVersion == IrVersion)
            || (context.Module.SchemaVersion == GuestLanguageOutcomeFlowValidator.SchemaVersion
                && context.Module.IrVersion == GuestLanguageOutcomeFlowValidator.IrVersion)
            || (context.Module.SchemaVersion == GuestLanguageErrorCatalogValidator.SchemaVersion
                && context.Module.IrVersion == GuestLanguageErrorCatalogValidator.IrVersion);
        if (!validVersion)
        {
            context.Add("ASIR1024", "Language outcome types require Guest IR 15/1.14 through 17/1.16.");
            return;
        }

        HashSet<string> seen = new(StringComparer.Ordinal);
        foreach (GuestLanguageOutcomeType declaration in declarations)
        {
            if (!seen.Add(declaration.TypeId)
                || !context.Types.TryGetValue(declaration.TypeId, out GuestType? type)
                || !HasValidShape(context, type, declaration.ValueTypeId))
            {
                context.Add("ASIR1024",
                    $"Language outcome type '{declaration.TypeId}' has an invalid or duplicate layout.");
            }
        }
    }

    private static bool HasValidShape(
        GuestValidationContext context,
        GuestType type,
        string? valueTypeId)
    {
        string[] fieldNames = valueTypeId is null
            ? new[] { "status", "error_type", "source", "error_root" }
            : new[] { "status", "error_type", "source", "error_root", "value" };
        if (type.Kind != "struct" || type.Storage != "memory"
            || !type.Fields.Select(field => field.Name).SequenceEqual(fieldNames, StringComparer.Ordinal))
            return false;

        bool IsI32(GuestField field) =>
            context.Types.TryGetValue(field.TypeId, out GuestType? fieldType)
            && fieldType.Kind == "scalar" && fieldType.Storage == "i32"
            && fieldType.Size == 4 && fieldType.Alignment == 4;
        if (!type.Fields.Take(3).All(IsI32)
            || !context.Types.TryGetValue(type.Fields[3].TypeId, out GuestType? root)
            || root.Kind != "managed_ref" || root.Storage != "i64"
            || root.Size != 8 || root.Alignment != 8
            || root.ElementTypeId is null)
            return false;

        if (valueTypeId is null)
            return true;
        return context.Types.TryGetValue(valueTypeId, out GuestType? valueType)
            && valueType.Kind is not ("void" or "borrowed_ref")
            && type.Fields[4].TypeId == valueTypeId;
    }
}

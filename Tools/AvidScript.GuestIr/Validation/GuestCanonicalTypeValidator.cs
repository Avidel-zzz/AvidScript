using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

internal static class GuestCanonicalTypeValidator
{
    public static void Validate(GuestValidationContext context)
    {
        GuestTypeLayoutResult result = GuestDataLayout.ComputeTypes(context.Module.Types);
        foreach (GuestDiagnostic diagnostic in result.Diagnostics)
        {
            context.Add(diagnostic.Code, diagnostic.Message);
        }

        if (!result.Succeeded)
        {
            return;
        }

        Dictionary<string, GuestType> canonicalTypes = result.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        foreach (GuestType actual in context.Module.Types)
        {
            if (!canonicalTypes.TryGetValue(actual.Id, out GuestType? canonical)
                || !HasCanonicalShape(actual, canonical))
            {
                context.Add(
                    "ASIR2005",
                    $"Type '{actual.Id}' does not use its canonical Guest memory layout.");
            }
        }
    }

    private static bool HasCanonicalShape(GuestType actual, GuestType canonical)
    {
        if (!string.Equals(actual.Kind, canonical.Kind, StringComparison.Ordinal)
            || !string.Equals(actual.Storage, canonical.Storage, StringComparison.Ordinal)
            || !string.Equals(actual.ElementTypeId, canonical.ElementTypeId, StringComparison.Ordinal)
            || !string.Equals(actual.UnderlyingTypeId, canonical.UnderlyingTypeId, StringComparison.Ordinal)
            || actual.Size != canonical.Size
            || actual.Alignment != canonical.Alignment
            || actual.Fields.Count != canonical.Fields.Count)
        {
            return false;
        }

        for (int index = 0; index < actual.Fields.Count; ++index)
        {
            GuestField actualField = actual.Fields[index];
            GuestField canonicalField = canonical.Fields[index];
            if (!string.Equals(actualField.Id, canonicalField.Id, StringComparison.Ordinal)
                || !string.Equals(actualField.Name, canonicalField.Name, StringComparison.Ordinal)
                || !string.Equals(actualField.TypeId, canonicalField.TypeId, StringComparison.Ordinal)
                || actualField.Offset != canonicalField.Offset)
            {
                return false;
            }
        }

        return true;
    }
}

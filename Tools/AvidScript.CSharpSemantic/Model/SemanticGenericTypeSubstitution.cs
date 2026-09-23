using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace AvidScript.CSharpSemantic;

public static class SemanticGenericTypeSubstitution
{
    // Only structural shapes already present in the Roslyn artifact can be
    // closed here. In particular, an arbitrary named generic type is not
    // reconstructed from its printed name.
    public static bool TryClose(
        string typeId,
        IReadOnlyDictionary<string, string> argumentsByParameter,
        IReadOnlyDictionary<string, SemanticType> types,
        IReadOnlyDictionary<string, SemanticTypeShape> shapes,
        out string closedTypeId)
    {
        if (argumentsByParameter.Keys.Any(parameterId =>
                !parameterId.StartsWith("type:", StringComparison.Ordinal)))
        {
            closedTypeId = typeId;
            return false;
        }
        return TryCloseCore(typeId, argumentsByParameter, types, shapes,
            new HashSet<string>(StringComparer.Ordinal), out closedTypeId);
    }

    private static bool TryCloseCore(
        string typeId,
        IReadOnlyDictionary<string, string> argumentsByParameter,
        IReadOnlyDictionary<string, SemanticType> types,
        IReadOnlyDictionary<string, SemanticTypeShape> shapes,
        ISet<string> visiting,
        out string closedTypeId)
    {
        if (argumentsByParameter.TryGetValue(typeId, out string? direct))
        {
            closedTypeId = direct;
            return types.ContainsKey(direct);
        }
        closedTypeId = typeId;
        if (!types.TryGetValue(typeId, out SemanticType? type)
            || type.Kind == "type_parameter")
            return false;
        if (!visiting.Add(typeId) || visiting.Count > 32)
            return false;
        if (type.Kind == "array"
            && shapes.TryGetValue(typeId, out SemanticTypeShape? shape)
            && shape.ElementTypeId is { } elementId)
        {
            if (!typeId.EndsWith("[]", StringComparison.Ordinal)
                || !TryCloseCore(elementId, argumentsByParameter, types, shapes, visiting,
                    out string closedElementId))
                return false;
            if (closedElementId == elementId)
                return true;
            string? matchedTypeId = null;
            foreach (SemanticTypeShape candidate in shapes.Values)
            {
                if (candidate.ElementTypeId != closedElementId
                    || !candidate.TypeId.EndsWith("[]", StringComparison.Ordinal)
                    || !types.TryGetValue(candidate.TypeId, out SemanticType? candidateType)
                    || candidateType.Kind != "array")
                    continue;
                if (matchedTypeId is not null)
                    return false;
                matchedTypeId = candidate.TypeId;
            }
            if (matchedTypeId is null)
                return false;
            closedTypeId = matchedTypeId;
            return true;
        }
        return !argumentsByParameter.Keys.Any(parameterId =>
            Regex.IsMatch(type.CanonicalName,
                @"(?<![A-Za-z0-9_])" + Regex.Escape(parameterId["type:".Length..])
                + @"(?![A-Za-z0-9_])", RegexOptions.CultureInvariant));
    }
}

using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticClassContractValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.ClassTypes is null || document.Types is null
            || document.Types.Any(type => type is null || string.IsNullOrWhiteSpace(type.Id))
            || document.Types.Select(type => type.Id).Distinct(StringComparer.Ordinal).Count() != document.Types.Count)
            return false;
        if (document.SchemaVersion < 24) return document.ClassTypes.Count == 0;
        if (!((document.SchemaVersion == 24 && document.SemanticVersion == "1.28")
                || (document.SchemaVersion == 25 && document.SemanticVersion == "1.29")
                || (document.SchemaVersion == 26 && document.SemanticVersion == "1.30")
                || (document.SchemaVersion == SemanticContract.CurrentSchemaVersion && document.SemanticVersion == SemanticContract.CurrentSemanticVersion))
            || document.ClassTypes.Any(type => type is null || string.IsNullOrWhiteSpace(type.TypeId))
            || document.ClassTypes.Select(type => type.TypeId).Distinct(StringComparer.Ordinal).Count() != document.ClassTypes.Count
            || document.ClassTypes.Count != document.Types.Count(type => type.Kind == "class")) return false;
        Dictionary<string, SemanticType> types = document.Types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        Dictionary<string, SemanticClassType> classes = document.ClassTypes.ToDictionary(type => type.TypeId, StringComparer.Ordinal);
        foreach (SemanticClassType item in document.ClassTypes)
        {
            if (!types.TryGetValue(item.TypeId, out SemanticType? type) || type.Kind != "class" || type.IsValueType
                || (item.TypeId == "type:object" ? item.BaseTypeId is not null
                    : item.BaseTypeId is null || !classes.ContainsKey(item.BaseTypeId) || item.BaseTypeId == item.TypeId)
                || item.InterfaceTypeIds is null
                || item.InterfaceTypeIds.Any(id => id is null || !types.TryGetValue(id, out SemanticType? implemented)
                    || implemented.Kind != "interface")
                || !item.InterfaceTypeIds.SequenceEqual(item.InterfaceTypeIds.Distinct(StringComparer.Ordinal)
                    .OrderBy(id => id, StringComparer.Ordinal))
                || item.IsStatic && (item.HasImplicitDefaultConstructor
                    || item.HasInstanceInitializers || item.HasImplicitInstanceStorage || item.HasVirtualMembers
                    || item.HasFinalizer || item.HasPrimaryConstructor || item.InterfaceTypeIds.Count != 0)
                || item.HasPrimaryConstructor && (!item.IsSourceDeclared || item.HasImplicitDefaultConstructor)
                || item.BaseTypeId is { } parent && (classes[parent].IsStatic || classes[parent].IsSealed)) return false;
        }
        // Walk each inheritance edge at most once; malformed input must not recurse or loop indefinitely.
        HashSet<string> complete = new(StringComparer.Ordinal);
        foreach (string id in classes.Keys)
        {
            HashSet<string> path = new(StringComparer.Ordinal);
            string? current = id;
            while (current is not null && !complete.Contains(current))
            {
                if (!path.Add(current)) return false;
                current = classes[current].BaseTypeId;
            }
            complete.UnionWith(path);
        }
        return true;
    }
}

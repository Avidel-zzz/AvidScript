using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.CSharpSemantic;

public static class SemanticDelegateContractValidator
{
    public static bool IsValid(SemanticDocument document)
    {
        if (document.DelegateTypes is null || document.Types is null
            || document.Types.Any(type => type is null || string.IsNullOrWhiteSpace(type.Id))
            || document.Types.Select(type => type.Id).Distinct(StringComparer.Ordinal).Count() != document.Types.Count)
            return false;
        if (document.SchemaVersion < 21) return document.DelegateTypes.Count == 0;
        if (document.DelegateTypes.Any(type => type is null)
            || document.DelegateTypes.Select(type => type.TypeId).Distinct(StringComparer.Ordinal).Count() != document.DelegateTypes.Count)
            return false;

        Dictionary<string, SemanticType> types = document.Types.ToDictionary(type => type.Id, StringComparer.Ordinal);
        if (document.DelegateTypes.Count != document.Types.Count(type => type.Kind == "delegate")) return false;
        foreach (SemanticDelegateType signature in document.DelegateTypes)
        {
            if (signature.TypeId is null || !types.TryGetValue(signature.TypeId, out SemanticType? type)
                || type.Kind != "delegate" || signature.ReturnTypeId is null || !types.ContainsKey(signature.ReturnTypeId)
                || signature.ReturnRefKind is not ("none" or "ref" or "ref_readonly")
                || (signature.ReturnTypeId == "type:void" && signature.ReturnRefKind != "none")
                || signature.Parameters is null || signature.Parameters.Any(parameter => parameter is null
                    || parameter.TypeId is null || parameter.TypeId == "type:void" || !types.ContainsKey(parameter.TypeId)
                    || parameter.RefKind is not ("none" or "ref" or "out" or "in" or "ref_readonly"))
                || !signature.Parameters.Select(parameter => parameter.Ordinal).SequenceEqual(Enumerable.Range(0, signature.Parameters.Count)))
                return false;
            if (signature.InvokeMethodSymbolId != SemanticDelegateType.GetInvokeId(signature.TypeId,
                signature.ReturnTypeId, signature.ReturnRefKind, signature.Parameters)) return false;
        }
        return true;
    }
}

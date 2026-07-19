using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal sealed record CSharpGuestResolvedStateContract(
    string Policy,
    int Version,
    IReadOnlyDictionary<string, CSharpGuestResolvedStateField> Fields);

internal sealed record CSharpGuestResolvedStateField(
    string Disposition,
    IReadOnlyList<string> FormerNames);

internal static class CSharpGuestStateContractResolver
{
    public static CSharpGuestResolvedStateContract Resolve(
        SemanticDocument document,
        string ownerTypeId)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentException.ThrowIfNullOrWhiteSpace(ownerTypeId);

        SemanticStateContract[] matches = (document.StateContracts ?? Array.Empty<SemanticStateContract>())
            .Where(contract => contract is not null
                && string.Equals(contract.OwnerTypeId, ownerTypeId, StringComparison.Ordinal))
            .ToArray();
        if (matches.Length != 1)
        {
            throw Invalid("ASSTATE1001", "State schema requires exactly one semantic state contract for its exported owner.");
        }

        SemanticStateContract contract = matches[0];
        if (contract.Policy is not ("compatible" or "explicit"))
        {
            throw Invalid("ASSTATE1001", "Semantic state contract policy is invalid.");
        }
        if (contract.Version is < 1 or > 65535)
        {
            throw Invalid("ASSTATE1003", "Semantic state contract version must be within 1..65535.");
        }

        SemanticType ownerType = document.Types.SingleOrDefault(type =>
                string.Equals(type.Id, ownerTypeId, StringComparison.Ordinal))
            ?? throw Invalid("ASSTATE1001", "Semantic state contract owner type is missing.");
        string ownerSymbolId = $"symbol:type:{ownerType.CanonicalName}";
        Dictionary<string, SemanticSymbol> ownerFields = document.Symbols
            .Where(symbol => symbol.Kind == "field"
                && string.Equals(symbol.ContainingSymbolId, ownerSymbolId, StringComparison.Ordinal))
            .ToDictionary(symbol => symbol.Id, StringComparer.Ordinal);
        if (contract.Fields is null || contract.Fields.Count != ownerFields.Count)
        {
            throw Invalid("ASSTATE1001", "Semantic state contract fields do not match its owner fields.");
        }

        Dictionary<string, CSharpGuestResolvedStateField> fields = new(StringComparer.Ordinal);
        foreach (SemanticStateFieldContract fieldContract in contract.Fields)
        {
            if (fieldContract is null
                || string.IsNullOrWhiteSpace(fieldContract.SymbolId)
                || fieldContract.Disposition is not ("implicit" or "persist" or "transient")
                || !ownerFields.ContainsKey(fieldContract.SymbolId)
                || fields.ContainsKey(fieldContract.SymbolId)
                || fieldContract.Aliases is null)
            {
                throw Invalid("ASSTATE1001", "Semantic state contract field metadata is invalid.");
            }

            string[] formerNames = fieldContract.Aliases
                .OrderBy(alias => alias, StringComparer.Ordinal)
                .ToArray();
            if (formerNames.Any(string.IsNullOrWhiteSpace)
                || formerNames.Distinct(StringComparer.Ordinal).Count() != formerNames.Length)
            {
                throw Invalid("ASSTATE1002", "Semantic state contract aliases must be non-empty and unique.");
            }

            fields.Add(fieldContract.SymbolId, new CSharpGuestResolvedStateField(
                fieldContract.Disposition,
                formerNames));
        }

        HashSet<string> stableIds = new(StringComparer.Ordinal);
        foreach (KeyValuePair<string, CSharpGuestResolvedStateField> entry in fields)
        {
            SemanticSymbol field = ownerFields[entry.Key];
            if (!stableIds.Add(StableId(ownerTypeId, field.Name)))
            {
                throw Invalid("ASSTATE1002", "Semantic state contract current field identities must be unique.");
            }
        }
        foreach (KeyValuePair<string, CSharpGuestResolvedStateField> entry in fields)
        {
            foreach (string formerName in entry.Value.FormerNames)
            {
                if (!stableIds.Add(StableId(ownerTypeId, formerName)))
                {
                    throw Invalid("ASSTATE1002", "Semantic state contract aliases must not conflict with current or aliased state identities.");
                }
            }
        }

        return new CSharpGuestResolvedStateContract(contract.Policy, contract.Version, fields);
    }

    private static string StableId(string ownerTypeId, string name) => $"state:{ownerTypeId}:{name}";

    private static InvalidDataException Invalid(string code, string message) =>
        new($"{code}: {message}");
}

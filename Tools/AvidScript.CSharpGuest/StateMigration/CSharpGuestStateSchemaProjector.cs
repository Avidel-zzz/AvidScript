using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

public static class CSharpGuestStateSchemaProjector
{
    private static readonly JsonSerializerOptions FingerprintOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        WriteIndented = false,
    };

    public static CSharpGuestStateSchema Project(SemanticDocument document, GuestModule module)
    {
        ArgumentNullException.ThrowIfNull(document);
        ArgumentNullException.ThrowIfNull(module);

        IReadOnlyDictionary<string, SemanticCallable> callablesById = document.Callables.ToDictionary(
            callable => callable.MethodSymbolId,
            StringComparer.Ordinal);
        IEnumerable<string> ownerRootIds = document.Reachability?.RootCallableIds
            ?? document.Callables
                .Where(callable => callable.Export is not null)
                .Select(callable => callable.MethodSymbolId);
        string[] ownerTypeIds = ownerRootIds
            .Select(rootId => callablesById[rootId].ContainingTypeId)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToArray();
        if (ownerTypeIds.Length != 1)
        {
            throw new InvalidDataException(
                $"State schema requires exactly one exported script owner, found {ownerTypeIds.Length}.");
        }

        string ownerTypeId = ownerTypeIds[0];
        CSharpGuestResolvedStateContract contract = CSharpGuestStateContractResolver.Resolve(
            document,
            ownerTypeId);
        SemanticType ownerType = document.Types.Single(type =>
            string.Equals(type.Id, ownerTypeId, StringComparison.Ordinal));
        string ownerSymbolId = $"symbol:type:{ownerType.CanonicalName}";
        IReadOnlyDictionary<string, GuestType> types = module.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        IReadOnlyDictionary<string, GuestGlobal> globals = module.Globals.ToDictionary(
            global => global.Id,
            StringComparer.Ordinal);
        IReadOnlyDictionary<string, GuestStateSlot> slots = module.MemoryLayout.StateSlots.ToDictionary(
            slot => slot.GlobalId,
            StringComparer.Ordinal);
        Dictionary<string, string> fingerprintCache = new(StringComparer.Ordinal);
        List<CSharpGuestStateSlot> projected = new();

        foreach (SemanticSymbol field in document.Symbols
            .Where(symbol => symbol.Kind == "field"
                && symbol.IsStatic
                && string.Equals(symbol.ContainingSymbolId, ownerSymbolId, StringComparison.Ordinal))
            .OrderBy(symbol => symbol.Id, StringComparer.Ordinal))
        {
            if (field.IsConst || field.IsReadonly)
            {
                continue;
            }

            if (!contract.Fields.TryGetValue(field.Id, out CSharpGuestResolvedStateField? fieldContract))
            {
                throw new InvalidDataException($"ASSTATE1001: State field is missing semantic contract metadata: {field.Id}.");
            }
            if (fieldContract.Disposition == "transient"
                || (contract.Policy == "explicit" && fieldContract.Disposition != "persist"))
            {
                continue;
            }

            string globalId = CSharpGuestIds.Global(field.Id);
            string typeFingerprint = string.Empty;
            GuestStateSlot? slot = null;
            bool isSafe = field.TypeId is not null
                && TryFingerprintType(
                    field.TypeId,
                    types,
                    fingerprintCache,
                    new HashSet<string>(StringComparer.Ordinal),
                    out typeFingerprint)
                && globals.ContainsKey(globalId)
                && slots.TryGetValue(globalId, out slot);
            if (!isSafe)
            {
                if (fieldContract.Disposition == "persist")
                {
                    throw new InvalidDataException(
                        $"ASSTATE1005: Persisted field is not a safe Guest value slot: {field.Id}.");
                }

                continue;
            }

            projected.Add(new CSharpGuestStateSlot(
                $"state:{ownerTypeId}:{field.Name}",
                fieldContract.FormerNames
                    .Select(formerName => $"state:{ownerTypeId}:{formerName}")
                    .OrderBy(alias => alias, StringComparer.Ordinal)
                    .ToArray(),
                typeFingerprint,
                slot!.Offset,
                slot.Size,
                slot.Alignment));
        }

        projected.Sort((left, right) => StringComparer.Ordinal.Compare(left.StableId, right.StableId));
        return new CSharpGuestStateSchema(
            2,
            "host_snapshot",
            contract.Policy,
            contract.Version,
            ownerTypeId,
            projected);
    }

    private static bool TryFingerprintType(
        string typeId,
        IReadOnlyDictionary<string, GuestType> types,
        Dictionary<string, string> cache,
        HashSet<string> visiting,
        out string fingerprint)
    {
        if (cache.TryGetValue(typeId, out fingerprint!))
        {
            return true;
        }
        if (typeId == CSharpGuestIds.AddressTypeId
            || !types.TryGetValue(typeId, out GuestType? type)
            || !visiting.Add(typeId))
        {
            fingerprint = string.Empty;
            return false;
        }

        try
        {
            List<StateFieldFingerprint> fields = new();
            string? underlyingFingerprint = null;
            switch (type.Kind)
            {
                case "scalar" when type.Size > 0 && type.Alignment > 0:
                    break;
                case "enum" when type.UnderlyingTypeId is not null:
                    if (!TryFingerprintType(
                        type.UnderlyingTypeId,
                        types,
                        cache,
                        visiting,
                        out underlyingFingerprint))
                    {
                        fingerprint = string.Empty;
                        return false;
                    }
                    break;
                case "struct" when type.Size > 0 && type.Alignment > 0:
                    foreach (GuestField field in type.Fields
                        .OrderBy(field => field.Offset)
                        .ThenBy(field => field.Id, StringComparer.Ordinal))
                    {
                        if (!TryFingerprintType(
                            field.TypeId,
                            types,
                            cache,
                            visiting,
                            out string fieldFingerprint))
                        {
                            fingerprint = string.Empty;
                            return false;
                        }
                        fields.Add(new StateFieldFingerprint(
                            field.Id,
                            field.Offset,
                            fieldFingerprint));
                    }
                    break;
                default:
                    fingerprint = string.Empty;
                    return false;
            }

            StateTypeFingerprint shape = new(
                type.Id,
                type.Kind,
                type.Storage,
                type.Size,
                type.Alignment,
                fields,
                underlyingFingerprint);
            fingerprint = Convert.ToHexString(
                    SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(shape, FingerprintOptions)))
                .ToLowerInvariant();
            cache.Add(typeId, fingerprint);
            return true;
        }
        finally
        {
            visiting.Remove(typeId);
        }
    }

    private sealed record StateTypeFingerprint(
        [property: JsonPropertyOrder(0)] string Id,
        [property: JsonPropertyOrder(1)] string Kind,
        [property: JsonPropertyOrder(2)] string Storage,
        [property: JsonPropertyOrder(3)] int Size,
        [property: JsonPropertyOrder(4)] int Alignment,
        [property: JsonPropertyOrder(5)] IReadOnlyList<StateFieldFingerprint> Fields,
        [property: JsonPropertyOrder(6)] string? UnderlyingTypeFingerprint);

    private sealed record StateFieldFingerprint(
        [property: JsonPropertyOrder(0)] string Id,
        [property: JsonPropertyOrder(1)] int Offset,
        [property: JsonPropertyOrder(2)] string TypeFingerprint);
}

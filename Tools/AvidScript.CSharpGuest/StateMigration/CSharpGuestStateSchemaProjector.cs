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
        if (ownerTypeIds.Length > 0 && document.StaticInitialization is not null
            && ownerTypeIds.All(id => document.UeTypeDeclarations.Any(type => type.TypeId == id)))
            return ProjectGeneratedDomain(document, module);
        if (ownerTypeIds.Length != 1)
        {
            HashSet<string> ueTypeIds = document.UeTypeDeclarations
                .Select(declaration => declaration.TypeId)
                .ToHashSet(StringComparer.Ordinal);
            bool hasPrimarySourceMutableState = document.Symbols.Any(symbol =>
                symbol.Kind == "field"
                && symbol.IsStatic
                && !symbol.IsConst
                && !symbol.IsReadonly
                && !symbol.IsExecutableReferenceSource);
            if (ownerTypeIds.Length > 1
                && ownerTypeIds.All(ueTypeIds.Contains)
                && !hasPrimarySourceMutableState)
            {
                return new CSharpGuestStateSchema(
                    2,
                    "host_snapshot",
                    "implicit",
                    1,
                    module.ModuleId,
                    Array.Empty<CSharpGuestStateSlot>());
            }
            throw new InvalidDataException(
                $"State schema requires exactly one exported script owner, found {ownerTypeIds.Length}.");
        }

        return ProjectOwner(document, module, ownerTypeIds[0]);
    }

    private static CSharpGuestStateSchema ProjectGeneratedDomain(SemanticDocument document, GuestModule module)
    {
        if (!CSharpStaticSourcePreparation.TryPrepare(document, out _, out var storage, out string? error))
            throw new InvalidDataException("ASSTATE1001: Generated domain storage requires its source preparation: " + error);
        var shapes = document.TypeShapes.ToDictionary(shape => shape.TypeId, StringComparer.Ordinal);
        var owners = storage!.Types.Select(owner => ProjectOwner(document, module,
            shapes.TryGetValue(owner.TypeId, out var shape) && shape.GenericDefinitionTypeId is { } definition ? definition : owner.TypeId,
            owner.TypeId, storage.Fields)).ToArray();
        if (owners.Select(owner => owner.ContractVersion).Distinct().Count() != 1)
            throw new InvalidDataException("ASSTATE1003: Shared generated static owners require one domain state contract version.");
        string domainId = "execution_domain:" + module.ModuleId;
        var slots = owners.SelectMany(owner => owner.Slots.Select(slot => slot with
        {
            StableId = "state:" + domainId + ":" + slot.StableId["state:".Length..],
            Aliases = slot.Aliases.Select(alias => "state:" + domainId + ":" + alias["state:".Length..]).ToArray(),
        })).OrderBy(slot => slot.StableId, StringComparer.Ordinal).ToArray();
        return new(2, "host_snapshot", owners.All(owner => owner.Policy == "explicit") ? "explicit" : "compatible",
            owners[0].ContractVersion, domainId, slots);
    }

    private static CSharpGuestStateSchema ProjectOwner(SemanticDocument document, GuestModule module, string ownerTypeId,
        string? storageOwnerId = null, IReadOnlyList<CSharpStaticField>? preparedFields = null)
    {
        string stateOwnerId = storageOwnerId ?? ownerTypeId;
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
        bool validatedStaticPlan = document.StaticInitialization is not null
            && SemanticStaticInitializationValidator.IsValid(document);

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
            // Static normalization specializes storage per closed owner. Keep
            // the source field's stable migration identity, while resolving
            // only storage authorized by the validated source plan.
            if (validatedStaticPlan && document.StaticInitialization!.Types.Any(plan =>
                    plan.TypeId == ownerTypeId && plan.Fields.Any(item => item.FieldSymbolId == field.Id)))
                globalId = CSharpGuestIds.Global(CSharpStaticSourcePreparation.SpecializedFieldId(stateOwnerId, field.Id));
            string? fieldTypeId = preparedFields is null ? field.TypeId : preparedFields.SingleOrDefault(storage =>
                storage.OwnerTypeId == stateOwnerId && storage.SymbolId ==
                    CSharpStaticSourcePreparation.SpecializedFieldId(stateOwnerId, field.Id))?.TypeId;
            string typeFingerprint = string.Empty;
            GuestStateSlot? slot = null;
            bool isSafe = fieldTypeId is not null
                && TryFingerprintType(
                    fieldTypeId,
                    types,
                    fingerprintCache,
                    new HashSet<string>(StringComparer.Ordinal),
                    out typeFingerprint)
                && globals.TryGetValue(globalId, out GuestGlobal? global)
                && global.TypeId == fieldTypeId
                && slots.TryGetValue(globalId, out slot)
                && slot.TypeId == fieldTypeId;
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
                $"state:{stateOwnerId}:{field.Name}",
                fieldContract.FormerNames
                    .Select(formerName => $"state:{stateOwnerId}:{formerName}")
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
            stateOwnerId,
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

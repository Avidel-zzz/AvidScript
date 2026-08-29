using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;

namespace AvidScript.CSharpGuest;

internal enum CSharpLatentStorageCellKind
{
    Direct,
    Field,
    Address,
}

internal sealed record CSharpLatentStorageCell(
    CSharpLatentStorageCellKind Kind,
    string SourceTypeId,
    string StorageTypeId,
    IReadOnlyList<string> FieldPath);

internal sealed record CSharpLatentStorageArgumentPlan(
    IReadOnlyList<CSharpLatentStorageCell> Cells);

internal sealed record CSharpLatentStoragePlan(
    IReadOnlyList<CSharpLatentStorageArgumentPlan> Arguments);

internal static class CSharpLatentStoragePlanner
{
    private const int MaximumAggregateDepth = 8;
    private const int MaximumStorageCells = 64;

    public static bool TryBuildSingleValue(
        SemanticDocument document,
        SemanticOperation value,
        string storageTypeId,
        out CSharpLatentStorageArgumentPlan argumentPlan)
    {
        SemanticCallableParameter storage = new(
            0,
            "compiler:storage",
            "value",
            storageTypeId,
            "none");
        if (TryBuild(
                document,
                new[] { value },
                new[] { storage },
                out CSharpLatentStoragePlan? plan)
            && plan.Arguments.Count == 1)
        {
            argumentPlan = plan.Arguments[0];
            return true;
        }

        argumentPlan = null!;
        return false;
    }

    public static bool TryBuild(
        SemanticDocument document,
        IReadOnlyList<SemanticOperation> arguments,
        IReadOnlyList<SemanticCallableParameter> importParameters,
        out CSharpLatentStoragePlan plan)
    {
        Dictionary<string, SemanticType> types = document.Types
            .ToDictionary(type => type.Id, StringComparer.Ordinal);
        Dictionary<string, SemanticTypeShape> shapes = document.TypeShapes
            .ToDictionary(shape => shape.TypeId, StringComparer.Ordinal);
        List<CSharpLatentStorageArgumentPlan> argumentPlans = new(arguments.Count);
        int storageIndex = 0;
        foreach (SemanticOperation argument in arguments)
        {
            if (argument.TypeId is not { } argumentTypeId
                || !types.TryGetValue(argumentTypeId, out SemanticType? argumentType)
                || storageIndex >= importParameters.Count)
            {
                plan = null!;
                return false;
            }

            SemanticCallableParameter nextParameter = importParameters[storageIndex];
            if (IsDirectStorageType(argumentType, shapes, nextParameter))
            {
                argumentPlans.Add(new CSharpLatentStorageArgumentPlan(new[]
                {
                    new CSharpLatentStorageCell(
                        CSharpLatentStorageCellKind.Direct,
                        argumentTypeId,
                        nextParameter.TypeId,
                        Array.Empty<string>()),
                }));
                ++storageIndex;
                continue;
            }

            if (argumentType.Kind == "struct"
                && nextParameter.RefKind == "in"
                && nextParameter.TypeId == argumentTypeId)
            {
                argumentPlans.Add(new CSharpLatentStorageArgumentPlan(new[]
                {
                    new CSharpLatentStorageCell(
                        CSharpLatentStorageCellKind.Address,
                        CSharpGuestIds.AddressTypeId,
                        CSharpGuestIds.AddressTypeId,
                        Array.Empty<string>()),
                }));
                ++storageIndex;
                continue;
            }

            List<AggregateLeaf> leaves = new();
            if (argumentType.Kind != "struct"
                || !TryCollectAggregateLeaves(
                    document,
                    types,
                    argumentType,
                    Array.Empty<string>(),
                    0,
                    leaves)
                || leaves.Count == 0
                || leaves.Count > MaximumStorageCells
                || storageIndex + leaves.Count > importParameters.Count)
            {
                plan = null!;
                return false;
            }

            List<CSharpLatentStorageCell> cells = new(leaves.Count);
            foreach (AggregateLeaf leaf in leaves)
            {
                SemanticCallableParameter parameter = importParameters[storageIndex++];
                if (!types.TryGetValue(leaf.TypeId, out SemanticType? leafType)
                    || !IsDirectStorageType(leafType, shapes, parameter))
                {
                    plan = null!;
                    return false;
                }
                cells.Add(new CSharpLatentStorageCell(
                    CSharpLatentStorageCellKind.Field,
                    leaf.TypeId,
                    parameter.TypeId,
                    leaf.FieldPath));
            }
            argumentPlans.Add(new CSharpLatentStorageArgumentPlan(cells));
        }

        if (storageIndex != importParameters.Count)
        {
            plan = null!;
            return false;
        }
        plan = new CSharpLatentStoragePlan(argumentPlans);
        return true;
    }

    private static bool IsDirectStorageType(
        SemanticType publicType,
        IReadOnlyDictionary<string, SemanticTypeShape> shapes,
        SemanticCallableParameter storageParameter)
    {
        return IsDirectStorageType(
            publicType,
            shapes,
            storageParameter.TypeId,
            storageParameter.RefKind);
    }

    private static bool IsDirectStorageType(
        SemanticType publicType,
        IReadOnlyDictionary<string, SemanticTypeShape> shapes,
        string storageTypeId,
        string storageRefKind)
    {
        if (storageRefKind != "none")
        {
            return false;
        }
        if (publicType.Kind == "primitive")
        {
            return IsSupportedPrimitive(publicType.Id)
                && (storageTypeId == publicType.Id
                    || (publicType.Id == "type:bool"
                        && storageTypeId == "type:int32"));
        }
        return publicType.Kind == "enum"
            && shapes.TryGetValue(publicType.Id, out SemanticTypeShape? shape)
            && shape.EnumUnderlyingTypeId == storageTypeId;
    }

    private static bool TryCollectAggregateLeaves(
        SemanticDocument document,
        IReadOnlyDictionary<string, SemanticType> types,
        SemanticType type,
        IReadOnlyList<string> fieldPath,
        int depth,
        List<AggregateLeaf> leaves)
    {
        if (depth >= MaximumAggregateDepth)
        {
            return false;
        }
        string containingTypeId = $"symbol:type:{type.CanonicalName}";
        SemanticSymbol[] fields = document.Symbols
            .Where(symbol => symbol.Kind == "field"
                && !symbol.IsStatic
                && symbol.ContainingSymbolId == containingTypeId)
            .OrderBy(symbol => symbol.Span.Start)
            .ThenBy(symbol => symbol.Id, StringComparer.Ordinal)
            .ToArray();
        if (fields.Length == 0)
        {
            return false;
        }

        foreach (SemanticSymbol field in fields)
        {
            if (field.TypeId is not { } fieldTypeId
                || !types.TryGetValue(fieldTypeId, out SemanticType? fieldType))
            {
                return false;
            }
            string[] childPath = fieldPath.Append(field.Id).ToArray();
            if (fieldType.Kind == "struct")
            {
                if (!TryCollectAggregateLeaves(
                    document,
                    types,
                    fieldType,
                    childPath,
                    depth + 1,
                    leaves))
                {
                    return false;
                }
            }
            else if (fieldType.Kind is "primitive" or "enum")
            {
                leaves.Add(new AggregateLeaf(fieldTypeId, childPath));
                if (leaves.Count > MaximumStorageCells)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    private static bool IsSupportedPrimitive(string typeId)
    {
        return typeId is "type:bool"
            or "type:int8" or "type:uint8"
            or "type:int16" or "type:uint16"
            or "type:int32" or "type:uint32"
            or "type:int64" or "type:uint64"
            or "type:float32" or "type:float64";
    }

    private sealed record AggregateLeaf(
        string TypeId,
        IReadOnlyList<string> FieldPath);
}

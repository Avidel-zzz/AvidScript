using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpTypeLowerer
{
    public static CSharpTypeLoweringResult Lower(SemanticDocument document)
    {
        List<GuestDiagnostic> diagnostics = new();
        Dictionary<string, SemanticTypeShape> shapes = new(StringComparer.Ordinal);
        foreach (SemanticTypeShape shape in document.TypeShapes)
        {
            if (!shapes.TryAdd(shape.TypeId, shape))
            {
                Add(diagnostics, "ASCG1003", $"Semantic type shape '{shape.TypeId}' is duplicated.");
            }
        }

        Dictionary<string, SemanticType> semanticTypes = document.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        List<GuestType> rawTypes = new()
        {
            Scalar(CSharpGuestIds.AddressTypeId, "i32", 4, 4),
        };
        foreach (SemanticType type in document.Types.OrderBy(type => type.Id, StringComparer.Ordinal))
        {
            GuestType? lowered = LowerType(
                type,
                document.Symbols,
                document.Callables,
                shapes,
                semanticTypes,
                diagnostics);
            if (lowered is not null)
            {
                rawTypes.Add(lowered);
            }
        }

        if (diagnostics.Count != 0)
        {
            return Failure(diagnostics);
        }

        GuestTypeLayoutResult layout = GuestDataLayout.ComputeTypes(rawTypes);
        if (!layout.Succeeded)
        {
            foreach (GuestDiagnostic diagnostic in layout.Diagnostics)
            {
                Add(diagnostics, "ASCG1003", diagnostic.Message);
            }

            return Failure(diagnostics);
        }

        return new CSharpTypeLoweringResult(true, layout.Types, Array.Empty<GuestDiagnostic>());
    }

    private static GuestType? LowerType(
        SemanticType type,
        IReadOnlyList<SemanticSymbol> symbols,
        IReadOnlyList<SemanticCallable> callables,
        IReadOnlyDictionary<string, SemanticTypeShape> shapes,
        IReadOnlyDictionary<string, SemanticType> semanticTypes,
        List<GuestDiagnostic> diagnostics)
    {
        if (TryLowerIntrinsic(type, out GuestType? intrinsic))
        {
            return intrinsic;
        }

        if (CSharpObjectCapabilityPolicy.TryGetKind(type.CanonicalName, out CSharpObjectCapabilityKind capabilityKind))
        {
            if (!CSharpObjectCapabilityPolicy.IsType(type)
                || !CSharpObjectCapabilityPolicy.HasCanonicalField(type, symbols)
                || !CSharpObjectCapabilityPolicy.HasIntrinsicConstructor(type, symbols, callables))
            {
                Add(diagnostics, "ASCG1003", $"Object capability '{type.Id}' does not match the generated nominal wrapper contract.");
                return null;
            }

            return new GuestType(
                type.Id,
                capabilityKind == CSharpObjectCapabilityKind.Factory
                    ? "factory_ref"
                    : "object_type_ref",
                "i32",
                Array.Empty<GuestField>(),
                null,
                null,
                4,
                4);
        }

        if (CSharpClassReferencePolicy.IsType(type))
        {
            if (!CSharpClassReferencePolicy.HasCanonicalField(type, symbols)
                || !CSharpClassReferencePolicy.HasIntrinsicConstructor(type, symbols, callables))
            {
                Add(diagnostics, "ASCG1003", $"Class reference '{type.Id}' does not match the generated ordinal wrapper contract.");
                return null;
            }

            return new GuestType(
                type.Id,
                "class_ref",
                "i32",
                Array.Empty<GuestField>(),
                null,
                null,
                4,
                4);
        }

        switch (type.Kind)
        {
            case "struct":
                return LowerStruct(type, symbols);
            case "enum":
                if (!shapes.TryGetValue(type.Id, out SemanticTypeShape? enumShape)
                    || enumShape.EnumUnderlyingTypeId is null)
                {
                    Add(diagnostics, "ASCG1003", $"Enum '{type.Id}' has no underlying type shape.");
                    return null;
                }

                return new GuestType(
                    type.Id,
                    "enum",
                    "i32",
                    Array.Empty<GuestField>(),
                    null,
                    enumShape.EnumUnderlyingTypeId,
                    0,
                    1);
            case "array":
                if (!shapes.TryGetValue(type.Id, out SemanticTypeShape? arrayShape)
                    || arrayShape.ElementTypeId is null
                    || !semanticTypes.ContainsKey(arrayShape.ElementTypeId))
                {
                    Add(diagnostics, "ASCG1003", $"Array '{type.Id}' has no valid element type shape.");
                    return null;
                }

                return new GuestType(
                    type.Id,
                    "array",
                    "i32",
                    Array.Empty<GuestField>(),
                    arrayShape.ElementTypeId,
                    null,
                    0,
                    1);
            case "class":
            case "interface":
                return null;
            default:
                Add(diagnostics, "ASCG1003", $"Semantic type '{type.Id}' has unsupported kind '{type.Kind}'.");
                return null;
        }
    }

    private static bool TryLowerIntrinsic(SemanticType type, out GuestType? guestType)
    {
        guestType = type.CanonicalName switch
        {
            "void" => new GuestType(type.Id, "void", "none", Array.Empty<GuestField>(), null, null, 0, 1),
            "bool" or "int8" or "uint8" => Scalar(type.Id, "i32", 1, 1),
            "char16" or "int16" or "uint16" => Scalar(type.Id, "i32", 2, 2),
            "int32" or "uint32" => Scalar(type.Id, "i32", 4, 4),
            "int64" or "uint64" => Scalar(type.Id, "i64", 8, 8),
            "float32" => Scalar(type.Id, "f32", 4, 4),
            "float64" => Scalar(type.Id, "f64", 8, 8),
            "string" => new GuestType(type.Id, "string", "i32", Array.Empty<GuestField>(), null, null, 0, 1),
            _ => null,
        };
        return guestType is not null;
    }

    private static GuestType LowerStruct(SemanticType type, IReadOnlyList<SemanticSymbol> symbols)
    {
        string containingTypeId = $"symbol:type:{type.CanonicalName}";
        GuestField[] fields = symbols
            .Where(symbol => symbol.Kind == "field"
                && !symbol.IsStatic
                && string.Equals(symbol.ContainingSymbolId, containingTypeId, StringComparison.Ordinal))
            .OrderBy(symbol => symbol.Span.Start)
            .ThenBy(symbol => symbol.Id, StringComparer.Ordinal)
            .Select(symbol => new GuestField(symbol.Id, symbol.Name, symbol.TypeId!, 0))
            .ToArray();
        return new GuestType(type.Id, "struct", "memory", fields, null, null, 0, 1);
    }

    private static GuestType Scalar(string id, string storage, int size, int alignment)
    {
        return new GuestType(
            id,
            "scalar",
            storage,
            Array.Empty<GuestField>(),
            null,
            null,
            size,
            alignment);
    }

    private static CSharpTypeLoweringResult Failure(IEnumerable<GuestDiagnostic> diagnostics)
    {
        GuestDiagnostic[] ordered = diagnostics
            .OrderBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Message, StringComparer.Ordinal)
            .ToArray();
        return new CSharpTypeLoweringResult(false, Array.Empty<GuestType>(), ordered);
    }

    private static void Add(List<GuestDiagnostic> diagnostics, string code, string message)
    {
        diagnostics.Add(new GuestDiagnostic(code, "error", message, null));
    }
}

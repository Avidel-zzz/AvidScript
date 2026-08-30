using System;
using System.Collections.Generic;
using System.Linq;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;

namespace AvidScript.CSharpGuest;

internal static class CSharpTypeLowerer
{
    private const int MaximumAsyncStateFrameBytes = 4096;

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
        HashSet<string> ueTypeIds = document.UeTypeDeclarations
            .Select(type => type.TypeId)
            .ToHashSet(StringComparer.Ordinal);
        List<GuestType> rawTypes = new()
        {
            Scalar(CSharpGuestIds.AddressTypeId, "i32", 4, 4),
        };
        foreach (SemanticType type in document.Types.OrderBy(type => type.Id, StringComparer.Ordinal))
        {
            if (IsCompilerAsyncScaffoldType(document, type))
            {
                continue;
            }

            GuestType? lowered = LowerType(
                type,
                document.Symbols,
                document.Callables,
                shapes,
                semanticTypes,
                ueTypeIds,
                diagnostics);
            if (lowered is not null)
            {
                rawTypes.Add(lowered);
            }
        }
        if (document.UeTypeDeclarations.Count != 0)
        {
            AddTypeIfMissing(rawTypes, new GuestType(
                CSharpGuestIds.VoidTypeId,
                "void",
                "none",
                Array.Empty<GuestField>(),
                null,
                null,
                0,
                1));
            AddTypeIfMissing(rawTypes, Scalar(
                CSharpGuestIds.Float32TypeId,
                "f32",
                4,
                4));
        }

        foreach (SemanticAsyncStateFrame frame in document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Select(segment => segment.AwaitSite?.StateFrame)
            .Where(frame => frame is not null)
            .Cast<SemanticAsyncStateFrame>()
            .OrderBy(frame => frame.TypeId, StringComparer.Ordinal))
        {
            if (rawTypes.Any(type => type.Id == frame.TypeId)
                || frame.Slots.Count == 0
                || frame.Slots.Any(slot => !semanticTypes.ContainsKey(slot.TypeId))
                || frame.Slots.Select(slot => slot.SymbolId).Distinct(StringComparer.Ordinal).Count()
                    != frame.Slots.Count)
            {
                Add(diagnostics, "ASCG1020", $"Async state frame '{frame.TypeId}' is malformed.");
                continue;
            }
            rawTypes.Add(new GuestType(
                frame.TypeId,
                "struct",
                "memory",
                frame.Slots.Select(slot => new GuestField(
                    CSharpGuestIds.AsyncStateField(frame.TypeId, slot.SymbolId),
                    slot.SymbolId,
                    slot.TypeId,
                    0)).ToArray(),
                null,
                null,
                0,
                1));
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

        IReadOnlyDictionary<string, GuestType> laidOutTypes = layout.Types.ToDictionary(
            type => type.Id,
            StringComparer.Ordinal);
        foreach (SemanticAsyncStateFrame frame in document.AsyncMethods
            .SelectMany(method => method.Segments)
            .Select(segment => segment.AwaitSite?.StateFrame)
            .Where(frame => frame is not null)
            .Cast<SemanticAsyncStateFrame>())
        {
            if (!laidOutTypes.TryGetValue(frame.TypeId, out GuestType? frameType)
                || frameType.Size <= 0
                || frameType.Size > MaximumAsyncStateFrameBytes
                || frame.Slots.Any(slot => !IsStateTypeSupported(
                    slot.TypeId,
                    laidOutTypes,
                    new HashSet<string>(StringComparer.Ordinal),
                    allowReference: true)))
            {
                Add(diagnostics, "ASCG1020", $"Async state frame '{frame.TypeId}' is not a bounded fixed-value layout.");
            }
        }

        if (diagnostics.Count != 0)
        {
            return Failure(diagnostics);
        }

        return new CSharpTypeLoweringResult(true, layout.Types, Array.Empty<GuestDiagnostic>());
    }

    private static bool IsStateTypeSupported(
        string typeId,
        IReadOnlyDictionary<string, GuestType> types,
        ISet<string> visiting,
        bool allowReference)
    {
        if (!types.TryGetValue(typeId, out GuestType? type))
        {
            return false;
        }
        if (type.Kind is "scalar" or "enum" or "handle" or "class_ref" or "factory_ref" or "object_type_ref")
        {
            return type.Size > 0;
        }
        if (type.Kind == "array")
        {
            return allowReference
                && type.Storage == "i32"
                && type.Size == 4
                && type.Alignment == 4
                && type.ElementTypeId is not null;
        }
        if (type.Kind != "struct" || !visiting.Add(typeId))
        {
            return false;
        }
        bool supported = type.Size > 0
            && type.Fields.All(field => IsStateTypeSupported(
                field.TypeId,
                types,
                visiting,
                allowReference: false));
        visiting.Remove(typeId);
        return supported;
    }

    private static bool IsCompilerAsyncScaffoldType(
        SemanticDocument document,
        SemanticType type)
    {
        if (type.Kind == "type_parameter"
            || type.CanonicalName is
                "global::AvidScript.AvidOutcome<T>" or
                "global::AvidScript.AvidOutcomeAwaitable<T>" or
                "global::AvidScript.AvidOutcomeAwaiter<T>"
            || type.CanonicalName.StartsWith(
                "global::AvidScript.AvidOutcomeAwaitable<",
                StringComparison.Ordinal)
            || type.CanonicalName.StartsWith(
                "global::AvidScript.AvidOutcomeAwaiter<",
                StringComparison.Ordinal))
        {
            return true;
        }
        if (type.Kind != "delegate"
            || type.CanonicalName != "global::System.Action")
        {
            return false;
        }

        HashSet<string> scaffoldCallables = document.Callables
            .Where(callable =>
                (callable.ContainingTypeId is
                        "type:global::AvidScript.AvidDelayAwaiter" or
                        "type:global::AvidScript.AvidObjectAwaiter"
                    || callable.ContainingTypeId.StartsWith(
                        "type:global::AvidScript.AvidOutcomeAwaiter<",
                        StringComparison.Ordinal))
                && callable.MethodSymbolId.Contains(".OnCompleted(", StringComparison.Ordinal)
                && callable.Parameters.Count == 1
                && callable.Parameters[0].TypeId == type.Id)
            .Select(callable => callable.MethodSymbolId)
            .ToHashSet(StringComparer.Ordinal);
        if (scaffoldCallables.Count == 0)
        {
            return false;
        }

        bool usedByOtherCallable = document.Callables.Any(callable =>
            (callable.ReturnTypeId == type.Id
                || callable.Parameters.Any(parameter => parameter.TypeId == type.Id))
            && !scaffoldCallables.Contains(callable.MethodSymbolId));
        bool usedByOtherSymbol = document.Symbols.Any(symbol =>
            symbol.TypeId == type.Id
            && (symbol.ContainingSymbolId is null
                || !scaffoldCallables.Contains(symbol.ContainingSymbolId)));
        return !usedByOtherCallable && !usedByOtherSymbol;
    }

    private static GuestType? LowerType(
        SemanticType type,
        IReadOnlyList<SemanticSymbol> symbols,
        IReadOnlyList<SemanticCallable> callables,
        IReadOnlyDictionary<string, SemanticTypeShape> shapes,
        IReadOnlyDictionary<string, SemanticType> semanticTypes,
        IReadOnlySet<string> ueTypeIds,
        List<GuestDiagnostic> diagnostics)
    {
        if (TryLowerIntrinsic(type, out GuestType? intrinsic))
        {
            return intrinsic;
        }

        if (ueTypeIds.Contains(type.Id))
        {
            if (type.Kind != "class" || type.IsValueType)
            {
                Add(diagnostics, "ASCG1003", $"Script UE type '{type.Id}' is not a reference class.");
                return null;
            }
            return new GuestType(
                type.Id,
                "handle",
                "i64",
                Array.Empty<GuestField>(),
                null,
                null,
                8,
                8);
        }

        if (type.Kind == "struct"
            && shapes.TryGetValue(type.Id, out SemanticTypeShape? outcomeShape)
            && outcomeShape.GenericArgumentTypeId is { } outcomeValueTypeId)
        {
            if (!type.CanonicalName.StartsWith(
                    "global::AvidScript.AvidOutcome<",
                    StringComparison.Ordinal)
                || !semanticTypes.ContainsKey(outcomeValueTypeId)
                || !semanticTypes.ContainsKey(CSharpGuestIds.ContinuationStatusTypeId))
            {
                Add(diagnostics, "ASCG1003", $"Outcome '{type.Id}' has an invalid generic payload shape.");
                return null;
            }
            return new GuestType(
                type.Id,
                "struct",
                "memory",
                new[]
                {
                    new GuestField(
                        CSharpGuestIds.OutcomeStatusField(type.Id),
                        "Status",
                        CSharpGuestIds.ContinuationStatusTypeId,
                        0),
                    new GuestField(
                        CSharpGuestIds.OutcomeValueField(type.Id),
                        "Value",
                        outcomeValueTypeId,
                        0),
                },
                null,
                null,
                0,
                1);
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

        if (CSharpCompositeValueCapabilityPolicy.IsType(type))
        {
            if (!CSharpCompositeValueCapabilityPolicy.HasCanonicalField(type, symbols)
                || !CSharpCompositeValueCapabilityPolicy.HasIntrinsicConstructor(
                    type,
                    symbols,
                    callables))
            {
                Add(diagnostics, "ASCG1003", $"Composite value capability '{type.Id}' does not match the generated token wrapper contract.");
                return null;
            }

            return new GuestType(
                type.Id,
                "composite_ref",
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
                    4,
                    4);
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

    private static void AddTypeIfMissing(ICollection<GuestType> types, GuestType requiredType)
    {
        if (!types.Any(type => string.Equals(type.Id, requiredType.Id, StringComparison.Ordinal)))
        {
            types.Add(requiredType);
        }
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

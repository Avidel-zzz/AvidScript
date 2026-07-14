using System;
using System.Collections.Generic;
using System.Linq;

namespace AvidScript.GuestIr;

public static class GuestLayoutBuilder
{
    public static GuestLayoutResult Build(
        IReadOnlyList<GuestType> types,
        IReadOnlyList<GuestGlobal> globals,
        IReadOnlyList<GuestDataSegment> dataSegments,
        int stateStart = GuestLayoutMath.NullGuardSize)
    {
        ArgumentNullException.ThrowIfNull(types);
        ArgumentNullException.ThrowIfNull(globals);
        ArgumentNullException.ThrowIfNull(dataSegments);

        List<GuestDiagnostic> diagnostics = new();
        Dictionary<string, GuestType> typeMap = IndexTypes(types, diagnostics);
        GuestGlobal[] orderedGlobals = globals.OrderBy(global => global.Id, StringComparer.Ordinal).ToArray();
        GuestDataSegment[] orderedSegments = dataSegments
            .OrderBy(segment => segment.Id, StringComparer.Ordinal)
            .ToArray();
        ValidateInputs(typeMap, orderedGlobals, orderedSegments, stateStart, diagnostics);
        if (diagnostics.Count != 0)
        {
            return Failure(diagnostics);
        }

        try
        {
            int address = stateStart;
            GuestStateSlot[] stateSlots = new GuestStateSlot[orderedGlobals.Length];
            for (int index = 0; index < orderedGlobals.Length; ++index)
            {
                GuestGlobal global = orderedGlobals[index];
                GuestType type = typeMap[global.TypeId];
                address = GuestLayoutMath.AlignUp(address, type.Alignment);
                stateSlots[index] = new GuestStateSlot(
                    global.Id,
                    global.TypeId,
                    address,
                    type.Size,
                    type.Alignment);
                address = GuestLayoutMath.Add(address, type.Size);
            }

            int stateSize = checked(address - stateStart);
            int dataStart = GuestLayoutMath.AlignUp(address, GuestLayoutMath.RegionAlignment);
            address = dataStart;
            GuestDataSegment[] placedSegments = new GuestDataSegment[orderedSegments.Length];
            for (int index = 0; index < orderedSegments.Length; ++index)
            {
                GuestDataSegment segment = orderedSegments[index];
                address = GuestLayoutMath.AlignUp(address, segment.Alignment);
                placedSegments[index] = segment with { Address = address };
                address = GuestLayoutMath.Add(address, segment.Bytes.Count);
            }

            int dataEnd = address;
            int heapStart = GuestLayoutMath.AlignUp(address, GuestLayoutMath.RegionAlignment);
            GuestMemoryLayout layout = new(
                stateStart,
                stateSize,
                dataStart,
                dataEnd,
                heapStart,
                stateSlots);
            return new GuestLayoutResult(true, layout, placedSegments, Array.Empty<GuestDiagnostic>());
        }
        catch (OverflowException)
        {
            diagnostics.Add(new GuestDiagnostic(
                "ASIR2001",
                "error",
                "Guest memory layout exceeds the 32-bit address space.",
                null));
            return Failure(diagnostics);
        }
    }

    private static Dictionary<string, GuestType> IndexTypes(
        IReadOnlyList<GuestType> types,
        List<GuestDiagnostic> diagnostics)
    {
        Dictionary<string, GuestType> typeMap = new(StringComparer.Ordinal);
        foreach (GuestType type in types)
        {
            if (!typeMap.TryAdd(type.Id, type)
                || type.Size < 0
                || !GuestLayoutMath.IsValidAlignment(type.Alignment)
                || (type.Size > 0 && type.Size % type.Alignment != 0))
            {
                diagnostics.Add(new GuestDiagnostic(
                    "ASIR2005",
                    "error",
                    $"Type '{type.Id}' is duplicated or has a non-canonical layout.",
                    null));
            }
        }

        return typeMap;
    }

    private static void ValidateInputs(
        IReadOnlyDictionary<string, GuestType> typeMap,
        IReadOnlyList<GuestGlobal> globals,
        IReadOnlyList<GuestDataSegment> dataSegments,
        int stateStart,
        List<GuestDiagnostic> diagnostics)
    {
        if (stateStart < GuestLayoutMath.NullGuardSize)
        {
            diagnostics.Add(new GuestDiagnostic(
                "ASIR2005",
                "error",
                $"State start {stateStart} overlaps the reserved null guard.",
                null));
        }

        HashSet<string> globalIds = new(StringComparer.Ordinal);
        foreach (GuestGlobal global in globals)
        {
            if (!globalIds.Add(global.Id) || !typeMap.TryGetValue(global.TypeId, out GuestType? type))
            {
                diagnostics.Add(new GuestDiagnostic(
                    "ASIR2005",
                    "error",
                    $"Global '{global.Id}' is duplicated or references unknown type '{global.TypeId}'.",
                    null));
                continue;
            }

            if (type.Kind == "void" || !GuestConstantCodec.TryEncode(global.InitialValue, type, out _))
            {
                diagnostics.Add(new GuestDiagnostic(
                    "ASIR2005",
                    "error",
                    $"Global '{global.Id}' has an incompatible initializer.",
                    null));
            }
        }

        HashSet<string> segmentIds = new(StringComparer.Ordinal);
        foreach (GuestDataSegment segment in dataSegments)
        {
            if (!segmentIds.Add(segment.Id)
                || !typeMap.ContainsKey(segment.TypeId)
                || !GuestLayoutMath.IsValidAlignment(segment.Alignment)
                || segment.Bytes is null
                || segment.ElementCount < 0)
            {
                diagnostics.Add(new GuestDiagnostic(
                    "ASIR2005",
                    "error",
                    $"Data segment '{segment.Id}' has invalid identity, type, alignment, or payload.",
                    null));
            }
        }
    }

    private static GuestLayoutResult Failure(IEnumerable<GuestDiagnostic> diagnostics)
    {
        GuestDiagnostic[] ordered = diagnostics
            .OrderBy(diagnostic => diagnostic.Code, StringComparer.Ordinal)
            .ThenBy(diagnostic => diagnostic.Message, StringComparer.Ordinal)
            .ToArray();
        return new GuestLayoutResult(false, null, Array.Empty<GuestDataSegment>(), ordered);
    }
}

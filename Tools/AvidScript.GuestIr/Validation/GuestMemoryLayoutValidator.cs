using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace AvidScript.GuestIr;

internal static class GuestMemoryLayoutValidator
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    public static void Validate(GuestValidationContext context)
    {
        GuestMemoryLayout layout = context.Module.MemoryLayout;
        if (layout.StateStart < GuestLayoutMath.NullGuardSize
            || layout.StateSize < 0
            || layout.DataStart < 0
            || layout.DataEnd < layout.DataStart
            || layout.HeapStart < layout.DataEnd)
        {
            Add(context, "Guest memory regions have invalid bounds.");
            return;
        }

        try
        {
            ValidateState(context, layout);
            ValidateData(context, layout);
        }
        catch (OverflowException)
        {
            context.Add("ASIR2001", "Guest memory layout overflows the 32-bit address space.");
        }
    }

    private static void ValidateState(GuestValidationContext context, GuestMemoryLayout layout)
    {
        int stateEnd = GuestLayoutMath.Add(layout.StateStart, layout.StateSize);
        int expectedDataStart = GuestLayoutMath.AlignUp(stateEnd, GuestLayoutMath.RegionAlignment);
        if (layout.DataStart != expectedDataStart)
        {
            Add(context, "Guest data region does not begin after aligned state storage.");
        }

        Dictionary<string, GuestGlobal> globals = context.Module.Globals
            .GroupBy(global => global.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.First(), StringComparer.Ordinal);
        HashSet<string> slotIds = new(StringComparer.Ordinal);
        int previousEnd = layout.StateStart;
        string? previousId = null;
        foreach (GuestStateSlot slot in layout.StateSlots)
        {
            bool ordered = previousId is null
                || string.Compare(previousId, slot.GlobalId, StringComparison.Ordinal) < 0;
            previousId = slot.GlobalId;
            if (!ordered
                || !slotIds.Add(slot.GlobalId)
                || !globals.TryGetValue(slot.GlobalId, out GuestGlobal? global)
                || !context.Types.TryGetValue(slot.TypeId, out GuestType? type)
                || !string.Equals(global.TypeId, slot.TypeId, StringComparison.Ordinal)
                || slot.Size != type.Size
                || slot.Alignment != type.Alignment
                || slot.Offset != GuestLayoutMath.AlignUp(previousEnd, type.Alignment))
            {
                Add(context, $"State slot '{slot.GlobalId}' is missing, unordered, or inconsistent with its global.");
                continue;
            }

            previousEnd = GuestLayoutMath.Add(slot.Offset, slot.Size);
            if (previousEnd > stateEnd)
            {
                Add(context, $"State slot '{slot.GlobalId}' exceeds the state region.");
            }
        }

        if (slotIds.Count != globals.Count || previousEnd != stateEnd)
        {
            Add(context, "State slots do not cover every global with the canonical state size.");
        }
    }

    private static void ValidateData(GuestValidationContext context, GuestMemoryLayout layout)
    {
        int address = layout.DataStart;
        string? previousId = null;
        foreach (GuestDataSegment segment in context.Module.DataSegments)
        {
            bool ordered = previousId is null
                || string.Compare(previousId, segment.Id, StringComparison.Ordinal) < 0;
            previousId = segment.Id;
            int expectedAddress = GuestLayoutMath.AlignUp(address, segment.Alignment);
            if (!ordered || segment.Address != expectedAddress)
            {
                Add(context, $"Data segment '{segment.Id}' is unordered, overlapping, or misaligned.");
            }

            ValidateSegmentPayload(context, segment);
            address = GuestLayoutMath.Add(expectedAddress, segment.Bytes.Count);
        }

        if (layout.DataEnd != address
            || layout.HeapStart != GuestLayoutMath.AlignUp(address, GuestLayoutMath.RegionAlignment))
        {
            Add(context, "Guest data end or heap start is inconsistent with placed data segments.");
        }
    }

    private static void ValidateSegmentPayload(GuestValidationContext context, GuestDataSegment segment)
    {
        if (!context.Types.TryGetValue(segment.TypeId, out GuestType? type)
            || !GuestLayoutMath.IsValidAlignment(segment.Alignment)
            || segment.ElementCount < 0)
        {
            Add(context, $"Data segment '{segment.Id}' has invalid type or metadata.");
            return;
        }

        byte[] bytes = segment.Bytes.ToArray();
        if (segment.Kind == "utf8_string")
        {
            ValidateStringPayload(context, segment, type, bytes);
        }
        else if (segment.Kind == "constant_array")
        {
            ValidateArrayPayload(context, segment, type, bytes);
        }
        else
        {
            Add(context, $"Data segment '{segment.Id}' has unsupported kind '{segment.Kind}'.");
        }
    }

    private static void ValidateStringPayload(
        GuestValidationContext context,
        GuestDataSegment segment,
        GuestType type,
        byte[] bytes)
    {
        if (type.Kind != "string" || segment.Alignment != 4 || bytes.Length < 5)
        {
            Add(context, $"String segment '{segment.Id}' has invalid type, alignment, or size.");
            return;
        }

        int byteLength = BinaryPrimitives.ReadInt32LittleEndian(bytes);
        if (byteLength < 0
            || byteLength != segment.ElementCount
            || bytes.Length != GuestLayoutMath.Add(byteLength, 5)
            || bytes[^1] != 0)
        {
            Add(context, $"String segment '{segment.Id}' has an inconsistent UTF-8 header.");
            return;
        }

        try
        {
            StrictUtf8.GetString(bytes, 4, byteLength);
        }
        catch (DecoderFallbackException)
        {
            Add(context, $"String segment '{segment.Id}' contains invalid UTF-8.");
        }
    }

    private static void ValidateArrayPayload(
        GuestValidationContext context,
        GuestDataSegment segment,
        GuestType type,
        byte[] bytes)
    {
        if (type.Kind != "array"
            || type.ElementTypeId is null
            || !context.Types.TryGetValue(type.ElementTypeId, out GuestType? elementType)
            || bytes.Length < 4)
        {
            Add(context, $"Array segment '{segment.Id}' has invalid type or size.");
            return;
        }

        int count = BinaryPrimitives.ReadInt32LittleEndian(bytes);
        int payloadAlignment = Math.Max(4, elementType.Alignment);
        int payloadOffset = GuestLayoutMath.AlignUp(4, payloadAlignment);
        int stride = GuestLayoutMath.AlignUp(elementType.Size, elementType.Alignment);
        int expectedSize = GuestLayoutMath.Add(
            payloadOffset,
            GuestLayoutMath.Multiply(segment.ElementCount, stride));
        if (count != segment.ElementCount
            || segment.Alignment != payloadAlignment
            || bytes.Length != expectedSize)
        {
            Add(context, $"Array segment '{segment.Id}' has an inconsistent count, stride, or alignment.");
        }
    }

    private static void Add(GuestValidationContext context, string message)
    {
        context.Add("ASIR2005", message);
    }
}

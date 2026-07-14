using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace AvidScript.GuestIr;

public static class GuestDataLayout
{
    public static GuestTypeLayoutResult ComputeTypes(IReadOnlyList<GuestType> types)
    {
        ArgumentNullException.ThrowIfNull(types);
        return new GuestTypeLayoutResolver(types).Compute();
    }

    public static GuestDataEncodingResult CreateUtf8String(string id, string typeId, string value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        ArgumentException.ThrowIfNullOrWhiteSpace(typeId);
        ArgumentNullException.ThrowIfNull(value);

        try
        {
            byte[] payload = Encoding.UTF8.GetBytes(value);
            byte[] bytes = new byte[GuestLayoutMath.Add(payload.Length, 5)];
            BinaryPrimitives.WriteInt32LittleEndian(bytes, payload.Length);
            payload.CopyTo(bytes, 4);
            GuestDataSegment segment = new(
                id,
                "utf8_string",
                typeId,
                0,
                4,
                payload.Length,
                bytes);
            return new GuestDataEncodingResult(true, segment, Array.Empty<GuestDiagnostic>());
        }
        catch (OverflowException)
        {
            return Failure("ASIR2001", $"UTF-8 string data '{id}' exceeds the 32-bit address space.");
        }
    }

    public static GuestDataEncodingResult CreateConstantArray(
        string id,
        string arrayTypeId,
        IReadOnlyList<GuestConstant> elements,
        IReadOnlyList<GuestType> types)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(id);
        ArgumentException.ThrowIfNullOrWhiteSpace(arrayTypeId);
        ArgumentNullException.ThrowIfNull(elements);
        ArgumentNullException.ThrowIfNull(types);

        Dictionary<string, GuestType> typeMap = new(StringComparer.Ordinal);
        foreach (GuestType type in types)
        {
            if (!typeMap.TryAdd(type.Id, type))
            {
                return Failure("ASIR2004", $"Array data '{id}' received duplicate type '{type.Id}'.");
            }
        }

        if (!typeMap.TryGetValue(arrayTypeId, out GuestType? arrayType)
            || !string.Equals(arrayType.Kind, "array", StringComparison.Ordinal)
            || arrayType.ElementTypeId is null
            || !typeMap.TryGetValue(arrayType.ElementTypeId, out GuestType? elementType)
            || elementType.Kind is not ("scalar" or "enum" or "handle"))
        {
            return Failure("ASIR2004", $"Array data '{id}' has an invalid array or element type.");
        }

        try
        {
            int payloadAlignment = Math.Max(4, elementType.Alignment);
            int payloadOffset = GuestLayoutMath.AlignUp(4, payloadAlignment);
            int stride = GuestLayoutMath.AlignUp(elementType.Size, elementType.Alignment);
            int payloadSize = GuestLayoutMath.Multiply(elements.Count, stride);
            byte[] bytes = new byte[GuestLayoutMath.Add(payloadOffset, payloadSize)];
            BinaryPrimitives.WriteInt32LittleEndian(bytes, elements.Count);
            for (int index = 0; index < elements.Count; ++index)
            {
                if (!GuestConstantCodec.TryEncode(elements[index], elementType, out byte[] encoded))
                {
                    return Failure(
                        "ASIR2004",
                        $"Array data '{id}' element {index} is incompatible with '{elementType.Id}'.");
                }

                encoded.CopyTo(bytes, payloadOffset + (index * stride));
            }

            GuestDataSegment segment = new(
                id,
                "constant_array",
                arrayTypeId,
                0,
                payloadAlignment,
                elements.Count,
                bytes);
            return new GuestDataEncodingResult(true, segment, Array.Empty<GuestDiagnostic>());
        }
        catch (OverflowException)
        {
            return Failure("ASIR2001", $"Array data '{id}' exceeds the 32-bit address space.");
        }
    }

    private static GuestDataEncodingResult Failure(string code, string message)
    {
        GuestDiagnostic diagnostic = new(code, "error", message, null);
        return new GuestDataEncodingResult(false, null, new[] { diagnostic });
    }
}

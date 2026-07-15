using System;
using AvidScript.GuestIr;

namespace AvidScript.WasmBackend;

internal static class WasmMemoryEmitter
{
    public static void WriteLoad(
        WasmBinaryWriter writer,
        GuestType type,
        uint offset = 0)
    {
        (byte opcode, uint alignment) = type.Storage switch
        {
            "i32" when type.Size == 1 => ((byte)0x2d, 0u),
            "i32" when type.Size == 2 => ((byte)0x2f, 1u),
            "i32" => ((byte)0x28, 2u),
            "i64" => ((byte)0x29, 3u),
            "f32" => ((byte)0x2a, 2u),
            "f64" => ((byte)0x2b, 3u),
            _ => throw new NotSupportedException(
                $"Guest type '{type.Id}' cannot be loaded as a scalar."),
        };
        writer.WriteByte(opcode);
        writer.WriteU32(alignment);
        writer.WriteU32(offset);
    }

    public static void WriteStore(
        WasmBinaryWriter writer,
        GuestType type,
        uint offset = 0)
    {
        (byte opcode, uint alignment) = type.Storage switch
        {
            "i32" when type.Size == 1 => ((byte)0x3a, 0u),
            "i32" when type.Size == 2 => ((byte)0x3b, 1u),
            "i32" => ((byte)0x36, 2u),
            "i64" => ((byte)0x37, 3u),
            "f32" => ((byte)0x38, 2u),
            "f64" => ((byte)0x39, 3u),
            _ => throw new NotSupportedException(
                $"Guest type '{type.Id}' cannot be stored as a scalar."),
        };
        writer.WriteByte(opcode);
        writer.WriteU32(alignment);
        writer.WriteU32(offset);
    }

    public static void WriteCopy(
        WasmBinaryWriter writer,
        Action<WasmBinaryWriter> writeDestinationAddress,
        Action<WasmBinaryWriter> writeSourceAddress,
        int size,
        int destinationOffset = 0,
        int sourceOffset = 0)
    {
        ArgumentNullException.ThrowIfNull(writeDestinationAddress);
        ArgumentNullException.ThrowIfNull(writeSourceAddress);
        if (size < 0 || destinationOffset < 0 || sourceOffset < 0)
        {
            throw new OverflowException("Invalid WASM memory copy range.");
        }

        int copied = 0;
        while (copied < size)
        {
            int remaining = size - copied;
            int chunk = remaining >= 4 ? 4 : remaining >= 2 ? 2 : 1;
            writeDestinationAddress(writer);
            writeSourceAddress(writer);
            WriteChunkLoad(writer, chunk, checked((uint)(sourceOffset + copied)));
            WriteChunkStore(writer, chunk, checked((uint)(destinationOffset + copied)));
            copied += chunk;
        }
    }

    public static void WriteArrayElementAddress(
        WasmBinaryWriter writer,
        Action<WasmBinaryWriter> writeArrayAddress,
        Action<WasmBinaryWriter> writeIndex,
        GuestType arrayType,
        GuestType elementType)
    {
        ArgumentNullException.ThrowIfNull(writeArrayAddress);
        ArgumentNullException.ThrowIfNull(writeIndex);
        if (!string.Equals(arrayType.Kind, "array", StringComparison.Ordinal)
            || !string.Equals(arrayType.ElementTypeId, elementType.Id, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Array type '{arrayType.Id}' does not contain '{elementType.Id}'.");
        }

        int payloadOffset = AlignUp(4, elementType.Alignment);
        int stride = AlignUp(elementType.Size, elementType.Alignment);

        writeIndex(writer);
        writeArrayAddress(writer);
        writer.WriteByte(0x28);
        writer.WriteU32(2);
        writer.WriteU32(0);
        writer.WriteByte(0x49);
        writer.WriteByte(0x04);
        writer.WriteByte(0x40);
        writer.WriteByte(0x05);
        writer.WriteByte(0x00);
        writer.WriteByte(0x0b);

        writeArrayAddress(writer);
        if (payloadOffset != 0)
        {
            writer.WriteByte(0x41);
            writer.WriteS32(payloadOffset);
            writer.WriteByte(0x6a);
        }

        writeIndex(writer);
        if (stride != 1)
        {
            writer.WriteByte(0x41);
            writer.WriteS32(stride);
            writer.WriteByte(0x6c);
        }

        writer.WriteByte(0x6a);
    }

    private static int AlignUp(int value, int alignment)
    {
        if (value < 0
            || alignment <= 0
            || alignment > 16
            || (alignment & (alignment - 1)) != 0)
        {
            throw new OverflowException("Invalid WASM array element layout.");
        }

        return checked((value + alignment - 1) & -alignment);
    }
    private static void WriteChunkLoad(WasmBinaryWriter writer, int size, uint offset)
    {
        writer.WriteByte(size switch
        {
            4 => (byte)0x28,
            2 => (byte)0x2f,
            1 => (byte)0x2d,
            _ => throw new InvalidOperationException("Unsupported memory copy chunk."),
        });
        writer.WriteU32(size == 4 ? 2u : size == 2 ? 1u : 0u);
        writer.WriteU32(offset);
    }

    private static void WriteChunkStore(WasmBinaryWriter writer, int size, uint offset)
    {
        writer.WriteByte(size switch
        {
            4 => (byte)0x36,
            2 => (byte)0x3b,
            1 => (byte)0x3a,
            _ => throw new InvalidOperationException("Unsupported memory copy chunk."),
        });
        writer.WriteU32(size == 4 ? 2u : size == 2 ? 1u : 0u);
        writer.WriteU32(offset);
    }
}

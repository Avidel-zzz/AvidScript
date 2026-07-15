using System;
using System.Collections.Generic;
using System.Text;

namespace AvidScript.WasmBackend;

internal sealed class WasmBinaryWriter
{
    private readonly List<byte> bytes = new();

    public int Count => bytes.Count;

    public void WriteByte(byte value)
    {
        bytes.Add(value);
    }

    public void WriteBytes(ReadOnlySpan<byte> value)
    {
        for (int index = 0; index < value.Length; ++index)
        {
            bytes.Add(value[index]);
        }
    }

    public void WriteU32(uint value)
    {
        WriteBytes(WasmLeb128.EncodeU32(value));
    }

    public void WriteS32(int value)
    {
        WriteBytes(WasmLeb128.EncodeS32(value));
    }
    public void WriteS64(long value)
    {
        WriteBytes(WasmLeb128.EncodeS64(value));
    }

    public void WriteName(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        byte[] encoded = Encoding.UTF8.GetBytes(value);
        WriteU32(checked((uint)encoded.Length));
        WriteBytes(encoded);
    }

    public void WriteSection(byte id, Action<WasmBinaryWriter> writePayload)
    {
        ArgumentNullException.ThrowIfNull(writePayload);
        WasmBinaryWriter payload = new();
        writePayload(payload);
        WriteByte(id);
        WriteU32(checked((uint)payload.Count));
        WriteBytes(payload.ToArray());
    }

    public byte[] ToArray()
    {
        return bytes.ToArray();
    }
}

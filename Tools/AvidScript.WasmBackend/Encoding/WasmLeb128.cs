using System.Collections.Generic;

namespace AvidScript.WasmBackend;

public static class WasmLeb128
{
    public static byte[] EncodeU32(uint value)
    {
        List<byte> bytes = new(5);
        do
        {
            byte current = (byte)(value & 0x7f);
            value >>= 7;
            if (value != 0)
            {
                current |= 0x80;
            }

            bytes.Add(current);
        }
        while (value != 0);

        return bytes.ToArray();
    }

    public static byte[] EncodeS32(int value)
    {
        List<byte> bytes = new(5);
        bool hasMore;
        do
        {
            byte current = (byte)(value & 0x7f);
            value >>= 7;
            bool signBitSet = (current & 0x40) != 0;
            hasMore = !((value == 0 && !signBitSet) || (value == -1 && signBitSet));
            if (hasMore)
            {
                current |= 0x80;
            }

            bytes.Add(current);
        }
        while (hasMore);

        return bytes.ToArray();
    }

    public static byte[] EncodeS64(long value)
    {
        List<byte> bytes = new(10);
        bool hasMore;
        do
        {
            byte current = (byte)(value & 0x7f);
            value >>= 7;
            bool signBitSet = (current & 0x40) != 0;
            hasMore = !((value == 0 && !signBitSet) || (value == -1 && signBitSet));
            if (hasMore)
            {
                current |= 0x80;
            }

            bytes.Add(current);
        }
        while (hasMore);

        return bytes.ToArray();
    }
}

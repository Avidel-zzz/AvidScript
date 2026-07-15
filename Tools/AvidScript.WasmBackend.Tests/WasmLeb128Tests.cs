using System;
using System.Linq;
using AvidScript.WasmBackend;

internal static class WasmLeb128Tests
{
    public static int Run()
    {
        UnsignedEncodingIsCanonical();
        SignedEncodingIsCanonical();
        return 2;
    }

    private static void UnsignedEncodingIsCanonical()
    {
        Assert(WasmLeb128.EncodeU32(0).SequenceEqual(new byte[] { 0x00 }), "u32 zero");
        Assert(WasmLeb128.EncodeU32(127).SequenceEqual(new byte[] { 0x7f }), "u32 127");
        Assert(WasmLeb128.EncodeU32(128).SequenceEqual(new byte[] { 0x80, 0x01 }), "u32 128");
        Assert(WasmLeb128.EncodeU32(624485).SequenceEqual(new byte[] { 0xe5, 0x8e, 0x26 }), "u32 sample");
    }

    private static void SignedEncodingIsCanonical()
    {
        Assert(WasmLeb128.EncodeS32(0).SequenceEqual(new byte[] { 0x00 }), "s32 zero");
        Assert(WasmLeb128.EncodeS32(-1).SequenceEqual(new byte[] { 0x7f }), "s32 -1");
        Assert(WasmLeb128.EncodeS32(63).SequenceEqual(new byte[] { 0x3f }), "s32 63");
        Assert(WasmLeb128.EncodeS32(64).SequenceEqual(new byte[] { 0xc0, 0x00 }), "s32 64");
        Assert(WasmLeb128.EncodeS32(-624485).SequenceEqual(new byte[] { 0x9b, 0xf1, 0x59 }), "s32 sample");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

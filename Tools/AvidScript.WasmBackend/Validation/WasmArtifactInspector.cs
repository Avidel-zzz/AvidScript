using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace AvidScript.WasmBackend;

public static class WasmArtifactInspector
{
    private static readonly byte[] Header =
    {
        0x00, 0x61, 0x73, 0x6d,
        0x01, 0x00, 0x00, 0x00,
    };

    private static readonly byte[] BoundsTrapSequence =
    {
        0x49, 0x04, 0x40, 0x05, 0x00, 0x0b,
    };
    public static WasmArtifactInfo Inspect(ReadOnlySpan<byte> artifact)
    {
        if (artifact.Length < Header.Length || !artifact[..Header.Length].SequenceEqual(Header))
        {
            throw new InvalidDataException("Artifact is not a WASM 1.0 module.");
        }

        List<byte> sectionIds = new();
        List<WasmImportInfo> imports = new();
        List<WasmExportInfo> exports = new();
        List<WasmCustomSectionInfo> customSections = new();
        uint functionBodyCount = 0;
        uint dataSegmentCount = 0;
        uint boundsTrapCount = 0;
        int offset = Header.Length;
        byte previousStandardSection = 0;
        while (offset < artifact.Length)
        {
            byte sectionId = artifact[offset++];
            uint payloadLength = ReadU32(artifact, ref offset);
            int payloadEnd = checked(offset + (int)payloadLength);
            if (sectionId > 12 || payloadEnd > artifact.Length)
            {
                throw new InvalidDataException("WASM section header or payload is invalid.");
            }

            if (sectionId != 0)
            {
                if (sectionId <= previousStandardSection)
                {
                    throw new InvalidDataException("WASM standard sections are not strictly ordered.");
                }

                previousStandardSection = sectionId;
            }

            sectionIds.Add(sectionId);
            ReadOnlySpan<byte> payload = artifact[offset..payloadEnd];
            switch (sectionId)
            {
                case 0:
                    customSections.Add(ReadCustomSection(payload));
                    break;
                case 2:
                    imports.AddRange(ReadImports(payload));
                    break;
                case 7:
                    exports.AddRange(ReadExports(payload));
                    break;
                case 10:
                    (functionBodyCount, boundsTrapCount) = ReadFunctionBodies(payload);
                    break;
                case 11:
                    dataSegmentCount = ReadDataSegmentCount(payload);
                    break;
            }

            offset = payloadEnd;
        }

        return new WasmArtifactInfo(
            sectionIds,
            imports,
            exports,
            customSections,
            functionBodyCount,
            dataSegmentCount,
            boundsTrapCount);
    }

    private static WasmCustomSectionInfo ReadCustomSection(ReadOnlySpan<byte> payload)
    {
        int offset = 0;
        string name = ReadName(payload, ref offset);
        string text = Encoding.UTF8.GetString(payload[offset..]);
        return new WasmCustomSectionInfo(name, text);
    }

    private static IReadOnlyList<WasmImportInfo> ReadImports(ReadOnlySpan<byte> payload)
    {
        int offset = 0;
        uint count = ReadU32(payload, ref offset);
        List<WasmImportInfo> imports = new(checked((int)count));
        for (uint index = 0; index < count; ++index)
        {
            string module = ReadName(payload, ref offset);
            string name = ReadName(payload, ref offset);
            if (offset >= payload.Length)
            {
                throw new InvalidDataException("WASM import is truncated.");
            }

            byte kind = payload[offset++];
            if (kind != 0)
            {
                throw new InvalidDataException(
                    "Artifact inspector currently accepts only function imports.");
            }

            uint typeIndex = ReadU32(payload, ref offset);
            imports.Add(new WasmImportInfo(module, name, kind, typeIndex));
        }

        RequireEnd(payload, offset, "import");
        return imports;
    }

    private static IReadOnlyList<WasmExportInfo> ReadExports(ReadOnlySpan<byte> payload)
    {
        int offset = 0;
        uint count = ReadU32(payload, ref offset);
        List<WasmExportInfo> exports = new(checked((int)count));
        for (uint index = 0; index < count; ++index)
        {
            string name = ReadName(payload, ref offset);
            if (offset >= payload.Length)
            {
                throw new InvalidDataException("WASM export is truncated.");
            }

            byte kind = payload[offset++];
            uint itemIndex = ReadU32(payload, ref offset);
            exports.Add(new WasmExportInfo(name, kind, itemIndex));
        }

        RequireEnd(payload, offset, "export");
        return exports;
    }

    private static (uint Count, uint BoundsTrapCount) ReadFunctionBodies(
        ReadOnlySpan<byte> payload)
    {
        int offset = 0;
        uint count = ReadU32(payload, ref offset);
        uint boundsTrapCount = 0;
        for (uint index = 0; index < count; ++index)
        {
            uint bodyLength = ReadU32(payload, ref offset);
            int bodyEnd = checked(offset + (int)bodyLength);
            if (bodyEnd > payload.Length)
            {
                throw new InvalidDataException("WASM function body is truncated.");
            }

            boundsTrapCount = checked(
                boundsTrapCount + CountSequence(
                    payload[offset..bodyEnd],
                    BoundsTrapSequence));
            offset = bodyEnd;
        }

        RequireEnd(payload, offset, "code");
        return (count, boundsTrapCount);
    }

    private static uint CountSequence(
        ReadOnlySpan<byte> bytes,
        ReadOnlySpan<byte> sequence)
    {
        uint count = 0;
        for (int index = 0; index <= bytes.Length - sequence.Length; ++index)
        {
            if (bytes.Slice(index, sequence.Length).SequenceEqual(sequence))
            {
                ++count;
                index += sequence.Length - 1;
            }
        }

        return count;
    }
    private static uint ReadDataSegmentCount(ReadOnlySpan<byte> payload)
    {
        int offset = 0;
        uint count = ReadU32(payload, ref offset);
        for (uint index = 0; index < count; ++index)
        {
            uint flags = ReadU32(payload, ref offset);
            if (flags != 0 || offset >= payload.Length || payload[offset++] != 0x41)
            {
                throw new InvalidDataException(
                    "WASM data segment is not an active memory-zero segment.");
            }

            ReadS32(payload, ref offset);
            if (offset >= payload.Length || payload[offset++] != 0x0b)
            {
                throw new InvalidDataException(
                    "WASM data segment offset expression is malformed.");
            }

            uint byteCount = ReadU32(payload, ref offset);
            offset = checked(offset + (int)byteCount);
            if (offset > payload.Length)
            {
                throw new InvalidDataException("WASM data segment is truncated.");
            }
        }

        RequireEnd(payload, offset, "data");
        return count;
    }

    private static int ReadS32(ReadOnlySpan<byte> bytes, ref int offset)
    {
        int start = offset;
        int value = 0;
        int shift = 0;
        byte current = 0;
        for (int index = 0; index < 5; ++index)
        {
            if (offset >= bytes.Length)
            {
                throw new InvalidDataException("WASM signed LEB128 is truncated.");
            }

            current = bytes[offset++];
            value |= (current & 0x7f) << shift;
            shift += 7;
            if ((current & 0x80) == 0)
            {
                if (shift < 32 && (current & 0x40) != 0)
                {
                    value |= -1 << shift;
                }

                ReadOnlySpan<byte> encoded = bytes[start..offset];
                if (!encoded.SequenceEqual(WasmLeb128.EncodeS32(value)))
                {
                    throw new InvalidDataException(
                        "WASM signed LEB128 is not canonical.");
                }

                return value;
            }
        }

        throw new InvalidDataException("WASM signed LEB128 exceeds five bytes.");
    }
    private static string ReadName(ReadOnlySpan<byte> bytes, ref int offset)
    {
        uint length = ReadU32(bytes, ref offset);
        int end = checked(offset + (int)length);
        if (end > bytes.Length)
        {
            throw new InvalidDataException("WASM UTF-8 name is truncated.");
        }

        string value = Encoding.UTF8.GetString(bytes[offset..end]);
        offset = end;
        return value;
    }

    private static uint ReadU32(ReadOnlySpan<byte> bytes, ref int offset)
    {
        int start = offset;
        uint value = 0;
        int shift = 0;
        for (int index = 0; index < 5; ++index)
        {
            if (offset >= bytes.Length)
            {
                throw new InvalidDataException("WASM unsigned LEB128 is truncated.");
            }

            byte current = bytes[offset++];
            if (index == 4 && (current & 0xf0) != 0)
            {
                throw new InvalidDataException("WASM unsigned LEB128 exceeds u32.");
            }

            value |= (uint)(current & 0x7f) << shift;
            if ((current & 0x80) == 0)
            {
                ReadOnlySpan<byte> encoded = bytes[start..offset];
                if (!encoded.SequenceEqual(WasmLeb128.EncodeU32(value)))
                {
                    throw new InvalidDataException("WASM unsigned LEB128 is not canonical.");
                }

                return value;
            }

            shift += 7;
        }

        throw new InvalidDataException("WASM unsigned LEB128 exceeds five bytes.");
    }

    private static void RequireEnd(ReadOnlySpan<byte> payload, int offset, string sectionName)
    {
        if (offset != payload.Length)
        {
            throw new InvalidDataException(
                $"WASM {sectionName} section has trailing bytes.");
        }
    }
}
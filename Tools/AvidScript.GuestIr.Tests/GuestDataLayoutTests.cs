using System;
using System.Linq;
using AvidScript.GuestIr;

internal static class GuestDataLayoutTests
{
    public static int Run()
    {
        MixedStructLayoutUsesAlignmentAndTailPadding();
        EnumUsesUnderlyingIntegerLayout();
        RecursiveValueTypesAreRejected();
        Utf8StringUsesByteLengthHeaderAndTerminator();
        ConstantArrayUsesElementStrideAndLittleEndian();
        EmptyArrayHasOnlyItsLengthHeader();
        MemoryRegionsAndStateSlotsAreDeterministic();
        AddressOverflowFailsClosed();
        FinalMemoryLayoutIsValidated();
        FinalTypeLayoutMustBeCanonical();
        StructStateSupportsZeroInitialization();
        WideArrayAlignsPayloadAfterHeader();
        FinalDataSegmentPayloadIsValidated();
        return 13;
    }

    private static void MixedStructLayoutUsesAlignmentAndTailPadding()
    {
        GuestType byteType = Scalar("type:uint8", "i32", 1, 1);
        GuestType intType = Scalar("type:int32", "i32", 4, 4);
        GuestType shortType = Scalar("type:int16", "i32", 2, 2);
        GuestType structType = new(
            "type:mixed",
            "struct",
            "memory",
            new[]
            {
                new GuestField("field:a", "A", byteType.Id, 0),
                new GuestField("field:b", "B", intType.Id, 0),
                new GuestField("field:c", "C", shortType.Id, 0),
            },
            null,
            null,
            0,
            1);

        GuestTypeLayoutResult result = GuestDataLayout.ComputeTypes(
            new[] { byteType, intType, shortType, structType });
        GuestType layout = result.Types.Single(type => type.Id == structType.Id);

        Assert(result.Succeeded, "mixed struct layout should succeed");
        Assert(layout.Fields.Select(field => field.Offset).SequenceEqual(new[] { 0, 4, 8 }),
            "mixed struct fields should respect field alignment");
        Assert(layout.Size == 12 && layout.Alignment == 4,
            "mixed struct should include tail padding to struct alignment");
    }

    private static void EnumUsesUnderlyingIntegerLayout()
    {
        GuestType intType = Scalar("type:int32", "i32", 4, 4);
        GuestType enumType = new(
            "type:mode",
            "enum",
            "i32",
            Array.Empty<GuestField>(),
            null,
            intType.Id,
            0,
            1);

        GuestTypeLayoutResult result = GuestDataLayout.ComputeTypes(new[] { intType, enumType });
        GuestType layout = result.Types.Single(type => type.Id == enumType.Id);

        Assert(result.Succeeded, "enum layout should succeed");
        Assert(layout.Size == 4 && layout.Alignment == 4 && layout.Storage == "i32",
            "enum should inherit its underlying integer layout");
    }

    private static void RecursiveValueTypesAreRejected()
    {
        GuestType recursive = new(
            "type:recursive",
            "struct",
            "memory",
            new[] { new GuestField("field:self", "Self", "type:recursive", 0) },
            null,
            null,
            0,
            1);

        GuestTypeLayoutResult result = GuestDataLayout.ComputeTypes(new[] { recursive });

        Assert(!result.Succeeded, "recursive value layout should fail");
        Assert(result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASIR2002"),
            "recursive value layout should report ASIR2002");
    }

    private static void Utf8StringUsesByteLengthHeaderAndTerminator()
    {
        GuestDataEncodingResult result = GuestDataLayout.CreateUtf8String(
            "data:greeting",
            "type:string",
            "A中");

        Assert(result.Succeeded && result.Segment is not null, "UTF-8 string encoding should succeed");
        GuestDataSegment segment = result.Segment!;
        Assert(segment.Bytes.SequenceEqual(new byte[] { 4, 0, 0, 0, 65, 0xe4, 0xb8, 0xad, 0 }),
            "string data should use byte length, UTF-8 payload, and a zero terminator");
        Assert(segment.Alignment == 4 && segment.ElementCount == 4,
            "string segment should retain byte count and i32 header alignment");
    }

    private static void ConstantArrayUsesElementStrideAndLittleEndian()
    {
        GuestType shortType = Scalar("type:int16", "i32", 2, 2);
        GuestType arrayType = ArrayType("type:int16[]", shortType.Id);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(new[] { shortType, arrayType });

        GuestDataEncodingResult result = GuestDataLayout.CreateConstantArray(
            "data:values",
            arrayType.Id,
            new[] { new GuestConstant("int16", "1"), new GuestConstant("int16", "-2") },
            types.Types);

        Assert(result.Succeeded && result.Segment is not null, "constant array encoding should succeed");
        GuestDataSegment segment = result.Segment!;
        Assert(segment.Bytes.SequenceEqual(new byte[] { 2, 0, 0, 0, 1, 0, 0xfe, 0xff }),
            "array should use count header, element stride, and little-endian payload");
        Assert(segment.ElementCount == 2 && segment.Alignment == 4,
            "array metadata should retain element count and header alignment");
    }

    private static void EmptyArrayHasOnlyItsLengthHeader()
    {
        GuestType intType = Scalar("type:int32", "i32", 4, 4);
        GuestType arrayType = ArrayType("type:int32[]", intType.Id);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(new[] { intType, arrayType });

        GuestDataEncodingResult result = GuestDataLayout.CreateConstantArray(
            "data:empty",
            arrayType.Id,
            Array.Empty<GuestConstant>(),
            types.Types);

        Assert(result.Succeeded && result.Segment is not null, "empty array encoding should succeed");
        GuestDataSegment segment = result.Segment!;
        Assert(segment.Bytes.SequenceEqual(new byte[] { 0, 0, 0, 0 }),
            "empty array should contain only a zero length header");
    }

    private static void MemoryRegionsAndStateSlotsAreDeterministic()
    {
        GuestType byteType = Scalar("type:uint8", "i32", 1, 1);
        GuestType intType = Scalar("type:int32", "i32", 4, 4);
        GuestType stringType = new(
            "type:string",
            "string",
            "i32",
            Array.Empty<GuestField>(),
            null,
            null,
            0,
            1);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(new[] { byteType, intType, stringType });
        GuestGlobal[] globals =
        {
            new("global:z", byteType.Id, true, new GuestConstant("uint8", "1")),
            new("global:a", intType.Id, true, new GuestConstant("int32", "7")),
        };
        GuestDataSegment segment = GuestDataLayout.CreateUtf8String("data:text", stringType.Id, "x").Segment!;

        GuestLayoutResult first = GuestLayoutBuilder.Build(types.Types, globals, new[] { segment });
        GuestLayoutResult second = GuestLayoutBuilder.Build(types.Types, globals.Reverse().ToArray(), new[] { segment });

        Assert(first.Succeeded && second.Succeeded && first.Layout is not null && second.Layout is not null,
            "memory layout should succeed");
        GuestMemoryLayout firstLayout = first.Layout!;
        GuestMemoryLayout secondLayout = second.Layout!;
        Assert(firstLayout.StateStart == secondLayout.StateStart
            && firstLayout.StateSize == secondLayout.StateSize
            && firstLayout.DataStart == secondLayout.DataStart
            && firstLayout.DataEnd == secondLayout.DataEnd
            && firstLayout.HeapStart == secondLayout.HeapStart
            && firstLayout.StateSlots.SequenceEqual(secondLayout.StateSlots),
            "state layout should be independent of input collection order");
        Assert(firstLayout.StateStart == 16 && firstLayout.StateSlots[0].GlobalId == "global:a",
            "memory should reserve a null guard and sort state by stable global id");
        Assert(firstLayout.StateSlots.Select(slot => slot.Offset).SequenceEqual(new[] { 16, 20 }),
            "state slots should use deterministic aligned offsets");
        Assert(firstLayout.DataStart == 32 && first.DataSegments[0].Address == 32
            && firstLayout.HeapStart == 48,
            "data and heap regions should use 16-byte region alignment");
    }

    private static void AddressOverflowFailsClosed()
    {
        GuestType intType = Scalar("type:int64", "i64", 8, 8);
        GuestGlobal global = new("global:value", intType.Id, true, new GuestConstant("int64", "1"));

        GuestLayoutResult result = GuestLayoutBuilder.Build(
            new[] { intType },
            new[] { global },
            Array.Empty<GuestDataSegment>(),
            int.MaxValue - 3);

        Assert(!result.Succeeded, "address overflow should fail");
        Assert(result.Diagnostics.Any(diagnostic => diagnostic.Code == "ASIR2001"),
            "address overflow should report ASIR2001");
    }

    private static void FinalMemoryLayoutIsValidated()
    {
        GuestModule module = GuestModuleValidationTests.CreateMinimalModule();
        GuestLayoutResult layout = GuestLayoutBuilder.Build(
            module.Types,
            module.Globals,
            Array.Empty<GuestDataSegment>());
        GuestModule finalModule = module with
        {
            MemoryLayout = layout.Layout!,
            DataSegments = layout.DataSegments,
        };

        GuestValidationResult valid = GuestModuleValidator.Validate(finalModule);
        Assert(valid.Succeeded, "final laid-out module should validate");

        GuestMemoryLayout invalidLayout = layout.Layout! with { DataStart = 8 };
        GuestValidationResult invalid = GuestModuleValidator.Validate(
            finalModule with { MemoryLayout = invalidLayout });
        Assert(!invalid.Succeeded
            && invalid.Diagnostics.Any(diagnostic => diagnostic.Code == "ASIR2005"),
            "overlapping final memory regions should report ASIR2005");
    }
    private static void FinalTypeLayoutMustBeCanonical()
    {
        GuestModule module = GuestModuleValidationTests.CreateMinimalModule();
        GuestType byteType = Scalar("type:uint8", "i32", 1, 1);
        GuestType structType = new(
            "type:canonical",
            "struct",
            "memory",
            new[]
            {
                new GuestField("field:value", "Value", "type:int32", 0),
                new GuestField("field:flag", "Flag", byteType.Id, 0),
            },
            null,
            null,
            0,
            1);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(
            module.Types.Concat(new[] { byteType, structType }).ToArray());
        GuestLayoutResult layout = GuestLayoutBuilder.Build(
            types.Types,
            module.Globals,
            Array.Empty<GuestDataSegment>());
        GuestModule finalModule = module with
        {
            MemoryLayout = layout.Layout!,
            Types = types.Types,
            DataSegments = layout.DataSegments,
        };
        Assert(GuestModuleValidator.Validate(finalModule).Succeeded,
            "canonical final type layout should validate");

        GuestType canonical = types.Types.Single(type => type.Id == structType.Id);
        GuestField[] badFields = canonical.Fields.ToArray();
        badFields[1] = badFields[1] with { Offset = 1 };
        GuestType badStruct = canonical with { Fields = badFields };
        GuestType[] badTypes = types.Types
            .Select(type => type.Id == badStruct.Id ? badStruct : type)
            .ToArray();
        GuestValidationResult invalid = GuestModuleValidator.Validate(
            finalModule with { Types = badTypes });

        Assert(!invalid.Succeeded
            && invalid.Diagnostics.Any(diagnostic => diagnostic.Code == "ASIR2005"),
            "non-canonical final struct offsets should report ASIR2005");
    }
    private static void StructStateSupportsZeroInitialization()
    {
        GuestType intType = Scalar("type:int32", "i32", 4, 4);
        GuestType vectorType = new(
            "type:vector2",
            "struct",
            "memory",
            new[]
            {
                new GuestField("field:x", "X", intType.Id, 0),
                new GuestField("field:y", "Y", intType.Id, 0),
            },
            null,
            null,
            0,
            1);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(new[] { intType, vectorType });
        GuestGlobal state = new(
            "global:position",
            vectorType.Id,
            true,
            new GuestConstant("zero", null));

        GuestLayoutResult result = GuestLayoutBuilder.Build(
            types.Types,
            new[] { state },
            Array.Empty<GuestDataSegment>());

        Assert(result.Succeeded && result.Layout is not null
            && result.Layout.StateSlots[0].Size == 8,
            "struct state should support deterministic all-zero initialization");
    }
    private static void WideArrayAlignsPayloadAfterHeader()
    {
        GuestType longType = Scalar("type:int64", "i64", 8, 8);
        GuestType arrayType = ArrayType("type:int64[]", longType.Id);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(new[] { longType, arrayType });

        GuestDataEncodingResult result = GuestDataLayout.CreateConstantArray(
            "data:wide",
            arrayType.Id,
            new[] { new GuestConstant("int64", "72623859790382856") },
            types.Types);
        GuestDataSegment segment = result.Segment!;

        Assert(result.Succeeded && segment.Alignment == 8 && segment.Bytes.Count == 16,
            "wide array should align its payload after the i32 header");
        Assert(segment.Bytes.Skip(4).Take(4).All(value => value == 0)
            && segment.Bytes.Skip(8).SequenceEqual(new byte[] { 8, 7, 6, 5, 4, 3, 2, 1 }),
            "wide array payload should begin at offset 8 in little-endian order");
    }

    private static void FinalDataSegmentPayloadIsValidated()
    {
        GuestModule module = GuestModuleValidationTests.CreateMinimalModule();
        GuestType stringType = new(
            "type:string",
            "string",
            "i32",
            Array.Empty<GuestField>(),
            null,
            null,
            0,
            1);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(
            module.Types.Append(stringType).ToArray());
        GuestDataSegment segment = GuestDataLayout.CreateUtf8String(
            "data:text",
            stringType.Id,
            "x").Segment!;
        GuestLayoutResult layout = GuestLayoutBuilder.Build(
            types.Types,
            module.Globals,
            new[] { segment });
        GuestModule finalModule = module with
        {
            MemoryLayout = layout.Layout!,
            Types = types.Types,
            DataSegments = layout.DataSegments,
        };
        Assert(GuestModuleValidator.Validate(finalModule).Succeeded,
            "final string data segment should validate");

        byte[] corruptedBytes = layout.DataSegments[0].Bytes.ToArray();
        corruptedBytes[0] = 2;
        GuestDataSegment corrupted = layout.DataSegments[0] with { Bytes = corruptedBytes };
        GuestValidationResult invalid = GuestModuleValidator.Validate(
            finalModule with { DataSegments = new[] { corrupted } });

        Assert(!invalid.Succeeded
            && invalid.Diagnostics.Any(diagnostic => diagnostic.Code == "ASIR2005"),
            "tampered final string headers should report ASIR2005");
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

    private static GuestType ArrayType(string id, string elementTypeId)
    {
        return new GuestType(
            id,
            "array",
            "i32",
            Array.Empty<GuestField>(),
            elementTypeId,
            null,
            0,
            1);
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

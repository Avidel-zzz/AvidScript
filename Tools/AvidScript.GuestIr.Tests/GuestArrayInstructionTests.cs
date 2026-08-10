using System;
using System.Linq;
using AvidScript.GuestIr;

internal static class GuestArrayInstructionTests
{
    public static int Run()
    {
        ArrayLoadAndStoreAreTypeChecked();
        InvalidArrayIndexIsRejected();
        InvalidArrayLengthResultIsRejected();
        return 3;
    }

    private static void ArrayLoadAndStoreAreTypeChecked()
    {
        GuestValidationResult result = GuestModuleValidator.Validate(CreateArrayModule(false, false));

        Assert(result.Succeeded, "typed array length, load, and store should validate");
    }

    private static void InvalidArrayIndexIsRejected()
    {
        GuestValidationResult result = GuestModuleValidator.Validate(CreateArrayModule(true, false));

        Assert(!result.Succeeded, "non-i32 array index should be rejected");
        Assert(result.Diagnostics.Any(item => item.Code == "ASIR1008"),
            "invalid array index should report ASIR1008");
    }

    private static void InvalidArrayLengthResultIsRejected()
    {
        GuestValidationResult result = GuestModuleValidator.Validate(CreateArrayModule(false, true));

        Assert(!result.Succeeded, "non-i32 array length result should be rejected");
        Assert(result.Diagnostics.Any(item => item.Code == "ASIR1008"),
            "invalid array length result should report ASIR1008");
    }

    private static GuestModule CreateArrayModule(bool invalidIndex, bool invalidLength)
    {
        GuestModule module = GuestModuleValidationTests.CreateMinimalModule();
        GuestType arrayType = new(
            "type:int32_array",
            "array",
            "i32",
            Array.Empty<GuestField>(),
            "type:int32",
            null,
            4,
            4);
        GuestFunction function = new(
            "function:array_round_trip",
            new[]
            {
                new GuestRegister("value:array", arrayType.Id),
                new GuestRegister(
                    "value:index",
                    invalidIndex ? arrayType.Id : "type:int32"),
                new GuestRegister("value:input", "type:int32"),
            },
            new[]
            {
                new GuestRegister("value:result", "type:int32"),
                new GuestRegister(
                    "value:length",
                    invalidLength ? arrayType.Id : "type:int32"),
            },
            "type:int32",
            "block:entry",
            new[]
            {
                new GuestBasicBlock(
                    "block:entry",
                    new[]
                    {
                        new GuestInstruction(
                            "array_store",
                            null,
                            new[] { "value:array", "value:index", "value:input" },
                            "type:int32",
                            null,
                            null),
                        new GuestInstruction(
                            "array_load",
                            "value:result",
                            new[] { "value:array", "value:index" },
                            "type:int32",
                            null,
                            null),
                        new GuestInstruction(
                            "array_length",
                            "value:length",
                            new[] { "value:array" },
                            null,
                            null,
                            null),
                    },
                    new GuestTerminator("return", null, null, null, "value:result")),
            });
        return module with
        {
            Types = module.Types.Concat(new[] { arrayType }).ToArray(),
            Functions = new[] { function },
            Exports = new[] { new GuestExport("guest_array_round_trip", function.Id) },
        };
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

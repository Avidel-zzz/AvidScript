using System;
using System.Linq;
using AvidScript.GuestIr;

internal static class GuestModuleValidationTests
{
    public static int Run()
    {
        MinimalModuleIsValid();
        InvalidProvenanceFailsClosed();
        NullCollectionsFailClosed();
        NullRequiredStringsFailClosed();
        DuplicateIdsAreRejected();
        UnknownTypesAreRejected();
        UnknownValuesAreRejected();
        UnknownBlocksAndMalformedTerminatorsAreRejected();
        CallSignaturesAreValidated();
        InvalidConstantLiteralIsRejected();
        InstructionTypesMustMatch();
        VoidSemanticsUseTypeShape();
        ExportsMustBeUniqueAndTargetFunctions();
        return 13;
    }

    private static void MinimalModuleIsValid()
    {
        GuestValidationResult result = GuestModuleValidator.Validate(CreateMinimalModule());

        Assert(result.Succeeded, "minimal Guest IR module should validate");
        Assert(result.Diagnostics.Count == 0, "valid Guest IR should have no diagnostics");
    }

    private static void InvalidProvenanceFailsClosed()
    {
        GuestModule module = CreateMinimalModule() with
        {
            Provenance = CreateMinimalModule().Provenance with { SemanticSha256 = string.Empty },
        };

        AssertDiagnostic(module, "ASIR1001");
    }

    private static void NullCollectionsFailClosed()
    {
        GuestModule module = CreateMinimalModule() with { Types = null! };

        AssertDiagnostic(module, "ASIR1001");
    }
    private static void NullRequiredStringsFailClosed()
    {
        GuestModule module = CreateMinimalModule();
        GuestType invalidType = module.Types[0] with { Id = null! };

        AssertDiagnostic(module with { Types = new[] { invalidType, module.Types[1] } }, "ASIR1001");
    }
    private static void DuplicateIdsAreRejected()
    {
        GuestModule module = CreateMinimalModule();
        module = module with { Types = module.Types.Append(module.Types[1]).ToArray() };

        AssertDiagnostic(module, "ASIR1002");
    }

    private static void UnknownTypesAreRejected()
    {
        GuestModule module = CreateMinimalModule();
        GuestFunction function = module.Functions[0] with
        {
            Locals = new[] { new GuestRegister("value:result", "type:missing") },
        };

        AssertDiagnostic(module with { Functions = new[] { function } }, "ASIR1003");
    }

    private static void UnknownValuesAreRejected()
    {
        GuestModule module = CreateMinimalModule();
        GuestFunction function = module.Functions[0];
        GuestBasicBlock block = function.Blocks[0] with
        {
            Instructions = new[]
            {
                new GuestInstruction(
                    "copy",
                    "value:result",
                    new[] { "value:missing" },
                    null,
                    null,
                    null),
            },
        };
        function = function with { Blocks = new[] { block } };

        AssertDiagnostic(module with { Functions = new[] { function } }, "ASIR1004");
    }

    private static void UnknownBlocksAndMalformedTerminatorsAreRejected()
    {
        GuestModule module = CreateMinimalModule();
        GuestFunction function = module.Functions[0];
        GuestBasicBlock unknownTarget = function.Blocks[0] with
        {
            Terminator = new GuestTerminator("branch", null, "block:missing", null, null),
        };
        AssertDiagnostic(
            module with { Functions = new[] { function with { Blocks = new[] { unknownTarget } } } },
            "ASIR1005");

        GuestBasicBlock missingTarget = function.Blocks[0] with
        {
            Terminator = new GuestTerminator("branch", null, null, null, null),
        };
        AssertDiagnostic(
            module with { Functions = new[] { function with { Blocks = new[] { missingTarget } } } },
            "ASIR1006");
    }

    private static void CallSignaturesAreValidated()
    {
        GuestModule module = CreateMinimalModule();
        GuestImport import = new(
            "import:host_value",
            "env",
            "host_value",
            new[] { "type:int32" },
            "type:int32");
        GuestFunction function = module.Functions[0];
        GuestBasicBlock block = function.Blocks[0] with
        {
            Instructions = new[]
            {
                new GuestInstruction(
                    "call",
                    "value:result",
                    Array.Empty<string>(),
                    import.Id,
                    null,
                    null),
            },
        };
        function = function with { Blocks = new[] { block } };

        AssertDiagnostic(
            module with { Imports = new[] { import }, Functions = new[] { function } },
            "ASIR1009");
    }

    private static void InvalidConstantLiteralIsRejected()
    {
        GuestModule module = CreateMinimalModule();
        GuestFunction function = module.Functions[0];
        GuestBasicBlock block = function.Blocks[0];
        GuestInstruction invalidConstant = block.Instructions[0] with
        {
            Constant = new GuestConstant("int32", "not-an-int"),
        };
        block = block with { Instructions = new[] { invalidConstant } };
        function = function with { Blocks = new[] { block } };

        AssertDiagnostic(module with { Functions = new[] { function } }, "ASIR1008");
    }
    private static void InstructionTypesMustMatch()
    {
        GuestModule module = CreateMinimalModule();
        GuestType floatType = new(
            "type:float32",
            "scalar",
            "f32",
            Array.Empty<GuestField>(),
            null,
            null,
            4,
            4);
        GuestFunction function = module.Functions[0] with
        {
            Parameters = new[] { new GuestRegister("value:source", floatType.Id) },
        };
        GuestBasicBlock block = function.Blocks[0] with
        {
            Instructions = new[]
            {
                new GuestInstruction(
                    "copy",
                    "value:result",
                    new[] { "value:source" },
                    null,
                    null,
                    null),
            },
        };
        function = function with { Blocks = new[] { block } };

        AssertDiagnostic(
            module with
            {
                Types = module.Types.Append(floatType).ToArray(),
                Functions = new[] { function },
            },
            "ASIR1008");
    }
    private static void VoidSemanticsUseTypeShape()
    {
        GuestModule module = CreateMinimalModule();
        GuestType unitType = module.Types[0] with { Id = "type:unit" };
        GuestImport import = new(
            "import:notify",
            "env",
            "notify",
            Array.Empty<string>(),
            unitType.Id);
        GuestFunction function = module.Functions[0];
        GuestBasicBlock block = function.Blocks[0] with
        {
            Instructions = new[]
            {
                new GuestInstruction("call", null, Array.Empty<string>(), import.Id, null, null),
                function.Blocks[0].Instructions[0],
            },
        };
        function = function with { Blocks = new[] { block } };

        GuestValidationResult result = GuestModuleValidator.Validate(module with
        {
            Types = new[] { unitType, module.Types[1] },
            Imports = new[] { import },
            Functions = new[] { function },
        });

        Assert(result.Succeeded, "void semantics should come from type shape, not a canonical type id");
    }
    private static void ExportsMustBeUniqueAndTargetFunctions()
    {
        GuestModule module = CreateMinimalModule() with
        {
            Exports = new[]
            {
                new GuestExport("avid_entry", "function:entry"),
                new GuestExport("avid_entry", "function:missing"),
            },
        };

        AssertDiagnostic(module, "ASIR1007");
    }

    internal static GuestModule CreateMinimalModule()
    {
        GuestType voidType = new(
            "type:void",
            "void",
            "none",
            Array.Empty<GuestField>(),
            null,
            null,
            0,
            1);
        GuestType intType = new(
            "type:int32",
            "scalar",
            "i32",
            Array.Empty<GuestField>(),
            null,
            null,
            4,
            4);
        GuestFunction entry = new(
            "function:entry",
            Array.Empty<GuestRegister>(),
            new[] { new GuestRegister("value:result", "type:int32") },
            "type:int32",
            "block:entry",
            new[]
            {
                new GuestBasicBlock(
                    "block:entry",
                    new[]
                    {
                        new GuestInstruction(
                            "constant",
                            "value:result",
                            Array.Empty<string>(),
                            null,
                            null,
                            new GuestConstant("int32", "7")),
                    },
                    new GuestTerminator("return", null, null, null, "value:result")),
            });
        string hash = new('a', 64);
        return new GuestModule(
            1,
            "1.0",
            "minimal",
            "csharp",
            new GuestProvenance("Scripts/Minimal.cs", hash, hash, hash, 4, "1.4"),
            true,
            new[] { voidType, intType },
            Array.Empty<GuestImport>(),
            Array.Empty<GuestGlobal>(),
            new[] { entry },
            new[] { new GuestExport("avid_entry", entry.Id) },
            Array.Empty<GuestDiagnostic>());
    }

    private static void AssertDiagnostic(GuestModule module, string code)
    {
        GuestValidationResult result = GuestModuleValidator.Validate(module);
        Assert(!result.Succeeded, $"Guest IR validation should fail with {code}");
        Assert(result.Diagnostics.Any(diagnostic => diagnostic.Code == code),
            $"Guest IR validation should contain {code}");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

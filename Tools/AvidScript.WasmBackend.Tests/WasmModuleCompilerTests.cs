using System;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmModuleCompilerTests
{
    public static int Run()
    {
        MinimalModuleHasCanonicalSectionsAndProvenance();
        CompilationIsByteDeterministic();
        InvalidGuestIrIsRejected();
        ImportsAndCallsUseStableFunctionIndices();
        TypedOwnerImportUsesExactI64Signature();
        ConditionalControlFlowAndScalarOperatorsCompile();
        StateStructPointersAndSRetUseLinearMemory();
        HeapAtPageBoundaryStillReservesRuntimeStack();
        return 8;
    }

    private static void MinimalModuleHasCanonicalSectionsAndProvenance()
    {
        WasmCompilationResult result = WasmModuleCompiler.Compile(CreateMinimalModule());
        WasmArtifactInfo info = WasmArtifactInspector.Inspect(result.Bytes);

        Assert(result.Succeeded && result.Diagnostics.Count == 0, "minimal module should compile");
        Assert(result.Bytes.Take(8).SequenceEqual(
            new byte[] { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 }),
            "WASM magic and version should be canonical");
        Assert(info.SectionIds.SequenceEqual(new byte[] { 0, 1, 3, 5, 6, 7, 10 }),
            "minimal sections should be ordered with provenance custom section first");
        Assert(info.Exports.Any(item => item.Name == "memory" && item.Kind == 2)
            && info.Exports.Any(item => item.Name == "guest_value" && item.Kind == 0),
            "memory and Guest function should be exported");
        Assert(info.CustomSections.Any(item => item.Name == "avidscript.provenance"
            && item.PayloadText.Contains(new string('a', 64), StringComparison.Ordinal)),
            "custom provenance should retain input hashes");
    }

    private static void CompilationIsByteDeterministic()
    {
        GuestModule module = CreateMinimalModule();
        byte[] first = WasmModuleCompiler.Compile(module).Bytes;
        byte[] second = WasmModuleCompiler.Compile(module).Bytes;

        Assert(first.SequenceEqual(second), "same Guest IR should produce identical WASM bytes");
    }

    private static void InvalidGuestIrIsRejected()
    {
        GuestModule invalid = CreateMinimalModule() with { SchemaVersion = 99 };

        WasmCompilationResult result = WasmModuleCompiler.Compile(invalid);

        Assert(!result.Succeeded && result.Bytes.Length == 0,
            "invalid Guest IR should not produce WASM bytes");
        Assert(result.Diagnostics.Any(item => item.Code == "ASWB1001"),
            "invalid Guest IR should report ASWB1001");
    }

    private static void ImportsAndCallsUseStableFunctionIndices()
    {
        GuestModule module = CreateMinimalModule();
        GuestImport import = new(
            "import:add", "env", "add",
            new[] { "type:int32", "type:int32" }, "type:int32");
        GuestFunction function = new(
            "function:call_add",
            Array.Empty<GuestRegister>(),
            new[]
            {
                new GuestRegister("value:left", "type:int32"),
                new GuestRegister("value:right", "type:int32"),
                new GuestRegister("value:sum", "type:int32"),
            },
            "type:int32",
            "block:entry",
            new[]
            {
                new GuestBasicBlock(
                    "block:entry",
                    new[]
                    {
                        Constant("value:left", 20),
                        Constant("value:right", 22),
                        new GuestInstruction(
                            "call", "value:sum", new[] { "value:left", "value:right" },
                            import.Id, null, null),
                    },
                    new GuestTerminator("return", null, null, null, "value:sum")),
            });
        module = module with
        {
            Imports = new[] { import },
            Functions = new[] { function },
            Exports = new[] { new GuestExport("guest_add", function.Id) },
        };

        WasmCompilationResult result = WasmModuleCompiler.Compile(module);
        Assert(result.Succeeded, "import call module should compile");
        WasmArtifactInfo info = WasmArtifactInspector.Inspect(result.Bytes);
        Assert(info.SectionIds.SequenceEqual(new byte[] { 0, 1, 2, 3, 5, 6, 7, 10 }),
            "import module should contain a canonically ordered import section");
        Assert(info.Imports.Count == 1
            && info.Imports[0].Module == "env"
            && info.Imports[0].Name == "add"
            && info.Imports[0].Kind == 0,
            "function import should retain its host module and name");
        Assert(info.Exports.Single(item => item.Name == "guest_add").Index == 1,
            "defined function index should follow imported functions");
    }

    private static void TypedOwnerImportUsesExactI64Signature()
    {
        GuestModule module = CreateMinimalModule();
        GuestType int64Type = new(
            "type:int64", "scalar", "i64", Array.Empty<GuestField>(), null, null, 8, 8);
        GuestImport ownerImport = new(
            "import:typed_owner", "avidscript", "avid_owner_get_handle",
            Array.Empty<string>(), int64Type.Id);
        module = module with
        {
            Types = module.Types.Concat(new[] { int64Type }).ToArray(),
            Imports = new[] { ownerImport },
        };

        WasmCompilationResult result = WasmModuleCompiler.Compile(module);
        Assert(result.Succeeded, "typed owner import module should compile");
        WasmArtifactInfo info = WasmArtifactInspector.Inspect(result.Bytes);
        WasmImportInfo import = info.Imports.Single(item => item.Name == "avid_owner_get_handle");
        Assert(import.Module == "avidscript" && import.Kind == 0,
            "typed owner import should retain the canonical host namespace");
        Assert(ReadFunctionResultType(result.Bytes, import.TypeIndex) == 0x7e,
            "typed owner import must use the exact WASM i64 result signature");
    }

    private static void ConditionalControlFlowAndScalarOperatorsCompile()
    {
        GuestModule module = CreateMinimalModule();
        GuestType boolType = new(
            "type:bool", "scalar", "i32", Array.Empty<GuestField>(), null, null, 1, 1);
        GuestFunction function = new(
            "function:max_plus_one",
            new[]
            {
                new GuestRegister("value:left", "type:int32"),
                new GuestRegister("value:right", "type:int32"),
            },
            new[]
            {
                new GuestRegister("value:condition", boolType.Id),
                new GuestRegister("value:one", "type:int32"),
                new GuestRegister("value:left_result", "type:int32"),
                new GuestRegister("value:right_result", "type:int32"),
            },
            "type:int32",
            "block:condition",
            new[]
            {
                new GuestBasicBlock(
                    "block:condition",
                    new[]
                    {
                        Constant("value:one", 1),
                        new GuestInstruction(
                            "binary", "value:condition", new[] { "value:left", "value:right" },
                            null, "greater_than", null),
                    },
                    new GuestTerminator(
                        "branch_if", "value:condition", "block:left", "block:right", null)),
                new GuestBasicBlock(
                    "block:left",
                    new[]
                    {
                        new GuestInstruction(
                            "binary", "value:left_result", new[] { "value:left", "value:one" },
                            null, "add", null),
                    },
                    new GuestTerminator("return", null, null, null, "value:left_result")),
                new GuestBasicBlock(
                    "block:right",
                    new[]
                    {
                        new GuestInstruction(
                            "binary", "value:right_result", new[] { "value:right", "value:one" },
                            null, "add", null),
                    },
                    new GuestTerminator("return", null, null, null, "value:right_result")),
            });
        module = module with
        {
            Types = module.Types.Concat(new[] { boolType }).ToArray(),
            Functions = new[] { function },
            Exports = new[] { new GuestExport("guest_max_plus_one", function.Id) },
        };

        WasmCompilationResult result = WasmModuleCompiler.Compile(module);

        Assert(result.Succeeded, "multi-block scalar function should compile");
        Assert(WasmArtifactInspector.Inspect(result.Bytes).FunctionBodyCount == 1,
            "compiled CFG should retain one defined function body");
    }

    private static void StateStructPointersAndSRetUseLinearMemory()
    {
        GuestModule module = CreateMinimalModule();
        GuestType addressType = new(
            "type:address", "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4);
        GuestType pairType = new(
            "type:pair",
            "struct",
            "memory",
            new[]
            {
                new GuestField("field:x", "X", "type:int32", 0),
                new GuestField("field:y", "Y", "type:int32", 4),
            },
            null,
            null,
            8,
            4);
        GuestGlobal global = new(
            "global:counter", "type:int32", true, new GuestConstant("int32", "5"));
        GuestStateSlot stateSlot = new(global.Id, global.TypeId, 16, 4, 4);
        GuestFunction makePair = new(
            "function:make_pair",
            Array.Empty<GuestRegister>(),
            new[]
            {
                new GuestRegister("value:pair", pairType.Id),
                new GuestRegister("value:seven", "type:int32"),
            },
            pairType.Id,
            "block:make",
            new[]
            {
                new GuestBasicBlock(
                    "block:make",
                    new[]
                    {
                        new GuestInstruction(
                            "stack_alloc", "value:pair", Array.Empty<string>(), null, null, null),
                        Constant("value:seven", 7),
                        new GuestInstruction(
                            "field_store", null, new[] { "value:pair", "value:seven" },
                            "field:x", null, null),
                    },
                    new GuestTerminator("return", null, null, null, "value:pair")),
            });
        GuestFunction consumePair = new(
            "function:consume_pair",
            Array.Empty<GuestRegister>(),
            new[]
            {
                new GuestRegister("value:pair", pairType.Id),
                new GuestRegister("value:address", addressType.Id),
                new GuestRegister("value:indirect", "type:int32"),
                new GuestRegister("value:state", "type:int32"),
                new GuestRegister("value:sum", "type:int32"),
                new GuestRegister("value:final", "type:int32"),
            },
            "type:int32",
            "block:consume",
            new[]
            {
                new GuestBasicBlock(
                    "block:consume",
                    new[]
                    {
                        new GuestInstruction(
                            "call", "value:pair", Array.Empty<string>(), makePair.Id, null, null),
                        new GuestInstruction(
                            "address_of", "value:address", Array.Empty<string>(),
                            "value:pair", null, null),
                        new GuestInstruction(
                            "indirect_load", "value:indirect", new[] { "value:address" },
                            "type:int32", null, null),
                        new GuestInstruction(
                            "global_load", "value:state", Array.Empty<string>(),
                            global.Id, null, null),
                        new GuestInstruction(
                            "binary", "value:sum", new[] { "value:state", "value:indirect" },
                            null, "add", null),
                        new GuestInstruction(
                            "global_store", null, new[] { "value:sum" },
                            global.Id, null, null),
                        new GuestInstruction(
                            "global_load", "value:final", Array.Empty<string>(),
                            global.Id, null, null),
                    },
                    new GuestTerminator("return", null, null, null, "value:final")),
            });
        module = module with
        {
            MemoryLayout = new GuestMemoryLayout(
                16, 4, 32, 32, 32, new[] { stateSlot }),
            Types = module.Types.Concat(new[] { addressType, pairType }).ToArray(),
            Globals = new[] { global },
            Functions = new[] { makePair, consumePair },
            Exports = new[] { new GuestExport("guest_consume_pair", consumePair.Id) },
        };

        WasmCompilationResult result = WasmModuleCompiler.Compile(module);

        Assert(result.Succeeded, "state, struct pointer, and sret module should compile");
        WasmArtifactInfo info = WasmArtifactInspector.Inspect(result.Bytes);
        Assert(info.SectionIds.SequenceEqual(new byte[] { 0, 1, 3, 5, 6, 7, 10, 11 }),
            "non-zero state initializer should add a canonical data section");
        Assert(info.FunctionBodyCount == 2 && info.DataSegmentCount == 1,
            "memory module should retain two bodies and one state initializer segment");
    }
    private static void HeapAtPageBoundaryStillReservesRuntimeStack()
    {
        GuestModule module = CreateMinimalModule();
        GuestType stringDeclaration = new(
            "type:string", "string", "i32", Array.Empty<GuestField>(), null, null, 0, 1);
        GuestType pairDeclaration = new(
            "type:frame_pair",
            "struct",
            "memory",
            new[] { new GuestField("field:value", "Value", "type:int32", 0) },
            null,
            null,
            0,
            1);
        GuestTypeLayoutResult types = GuestDataLayout.ComputeTypes(
            module.Types.Concat(new[] { stringDeclaration, pairDeclaration }).ToArray());
        GuestDataSegment segment = GuestDataLayout.CreateUtf8String(
            "data:page_boundary", stringDeclaration.Id, new string('x', 65515)).Segment!;
        GuestLayoutResult layout = GuestLayoutBuilder.Build(
            types.Types, module.Globals, new[] { segment });
        Assert(types.Succeeded && layout.Succeeded && layout.Layout?.HeapStart == 65536,
            "test fixture should produce a valid page-aligned heap");
        GuestFunction function = new(
            "function:page_boundary",
            Array.Empty<GuestRegister>(),
            new[]
            {
                new GuestRegister("value:frame_pair", pairDeclaration.Id),
                new GuestRegister("value:result", "type:int32"),
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
                            "stack_alloc", "value:frame_pair", Array.Empty<string>(),
                            null, null, null),
                        Constant("value:result", 7),
                    },
                    new GuestTerminator("return", null, null, null, "value:result")),
            });
        module = module with
        {
            Types = types.Types,
            MemoryLayout = layout.Layout!,
            DataSegments = layout.DataSegments,
            Functions = new[] { function },
            Exports = new[] { new GuestExport("guest_page_boundary", function.Id) },
        };

        WasmCompilationResult result = WasmModuleCompiler.Compile(module);

        Assert(result.Succeeded, "page-aligned heap module should compile");
        Assert(ReadInitialMemoryPages(result.Bytes) == 2,
            "backend should reserve one runtime stack page beyond a page-aligned heap");
    }

    private static uint ReadInitialMemoryPages(byte[] artifact)
    {
        int offset = 8;
        while (offset < artifact.Length)
        {
            byte sectionId = artifact[offset++];
            uint payloadLength = ReadU32(artifact, ref offset);
            int payloadEnd = checked(offset + (int)payloadLength);
            if (sectionId == 5)
            {
                Assert(ReadU32(artifact, ref offset) == 1, "memory section should have one entry");
                Assert(artifact[offset++] == 0, "memory should use a minimum-only limit");
                return ReadU32(artifact, ref offset);
            }

            offset = payloadEnd;
        }

        throw new InvalidOperationException("memory section was not found");
    }

    private static byte ReadFunctionResultType(byte[] artifact, uint typeIndex)
    {
        int offset = 8;
        while (offset < artifact.Length)
        {
            byte sectionId = artifact[offset++];
            uint payloadLength = ReadU32(artifact, ref offset);
            int payloadEnd = checked(offset + (int)payloadLength);
            if (sectionId != 1)
            {
                offset = payloadEnd;
                continue;
            }

            uint typeCount = ReadU32(artifact, ref offset);
            for (uint index = 0; index < typeCount; ++index)
            {
                Assert(artifact[offset++] == 0x60, "function type must use the WASM function marker");
                uint parameterCount = ReadU32(artifact, ref offset);
                offset = checked(offset + (int)parameterCount);
                uint resultCount = ReadU32(artifact, ref offset);
                Assert(resultCount <= 1, "AvidScript function imports use at most one WASM result");
                byte resultType = resultCount == 1 ? artifact[offset++] : (byte)0x40;
                if (index == typeIndex)
                {
                    return resultType;
                }
            }

            break;
        }

        throw new InvalidOperationException("import function type was not found");
    }

    private static uint ReadU32(byte[] bytes, ref int offset)
    {
        uint value = 0;
        int shift = 0;
        for (int index = 0; index < 5; ++index)
        {
            byte current = bytes[offset++];
            value |= (uint)(current & 0x7f) << shift;
            if ((current & 0x80) == 0)
            {
                return value;
            }

            shift += 7;
        }

        throw new InvalidOperationException("invalid u32 LEB128 in test artifact");
    }
    private static GuestInstruction Constant(string resultId, int value)
    {
        return new GuestInstruction(
            "constant", resultId, Array.Empty<string>(), null, null,
            new GuestConstant(
                "int32",
                value.ToString(System.Globalization.CultureInfo.InvariantCulture)));
    }
    internal static GuestModule CreateMinimalModule()
    {
        GuestType voidType = new(
            "type:void", "void", "none", Array.Empty<GuestField>(), null, null, 0, 1);
        GuestType intType = new(
            "type:int32", "scalar", "i32", Array.Empty<GuestField>(), null, null, 4, 4);
        GuestFunction function = new(
            "function:value",
            Array.Empty<GuestRegister>(),
            new[] { new GuestRegister("value:result", intType.Id) },
            intType.Id,
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
            new GuestMemoryLayout(16, 0, 16, 16, 16, Array.Empty<GuestStateSlot>()),
            new[] { voidType, intType },
            Array.Empty<GuestImport>(),
            Array.Empty<GuestGlobal>(),
            Array.Empty<GuestDataSegment>(),
            new[] { function },
            new[] { new GuestExport("guest_value", function.Id) },
            Array.Empty<GuestDiagnostic>());
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

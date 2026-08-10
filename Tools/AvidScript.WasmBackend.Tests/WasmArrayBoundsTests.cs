using System;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmArrayBoundsTests
{
    public static int Run()
    {
        ArrayAccessEmitsBoundsTrapBeforeAddressing();
        CapabilityArraysEmitStaticImportsAndRetainLinearFallbacks();
        return 2;
    }

    private static void CapabilityArraysEmitStaticImportsAndRetainLinearFallbacks()
    {
        GuestModule module = WasmModuleCompilerTests.CreateMinimalModule();
        GuestType pointDeclaration = new(
            "type:point",
            "struct",
            "memory",
            new[]
            {
                new GuestField("field:x", "X", "type:int32", 0),
                new GuestField("field:y", "Y", "type:int32", 0),
            },
            null,
            null,
            0,
            1);
        GuestType intArrayDeclaration = new(
            "type:int32_array",
            "array",
            "i32",
            Array.Empty<GuestField>(),
            "type:int32",
            null,
            0,
            1);
        GuestType pointArrayDeclaration = new(
            "type:point_array",
            "array",
            "i32",
            Array.Empty<GuestField>(),
            pointDeclaration.Id,
            null,
            0,
            1);
        GuestTypeLayoutResult layouts = GuestDataLayout.ComputeTypes(
            module.Types.Concat(new[]
            {
                pointDeclaration,
                intArrayDeclaration,
                pointArrayDeclaration,
            }).ToArray());
        Assert(layouts.Succeeded, "scalar and memory array fixture types should lay out");

        GuestFunction scalar = new(
            "function:scalar_array",
            new[]
            {
                new GuestRegister("value:scalar_array", intArrayDeclaration.Id),
                new GuestRegister("value:scalar_index", "type:int32"),
                new GuestRegister("value:scalar_input", "type:int32"),
            },
            new[]
            {
                new GuestRegister("value:scalar_result", "type:int32"),
                new GuestRegister("value:scalar_length", "type:int32"),
            },
            "type:int32",
            "block:scalar",
            new[]
            {
                new GuestBasicBlock(
                    "block:scalar",
                    new[]
                    {
                        new GuestInstruction(
                            "array_store", null,
                            new[] { "value:scalar_array", "value:scalar_index", "value:scalar_input" },
                            "type:int32", null, null),
                        new GuestInstruction(
                            "array_load", "value:scalar_result",
                            new[] { "value:scalar_array", "value:scalar_index" },
                            "type:int32", null, null),
                        new GuestInstruction(
                            "array_length", "value:scalar_length",
                            new[] { "value:scalar_array" },
                            null, null, null),
                    },
                    new GuestTerminator("return", null, null, null, "value:scalar_length")),
            });
        GuestFunction memory = new(
            "function:memory_array",
            new[]
            {
                new GuestRegister("value:memory_array", pointArrayDeclaration.Id),
                new GuestRegister("value:memory_index", "type:int32"),
                new GuestRegister("value:memory_input", pointDeclaration.Id),
            },
            new[] { new GuestRegister("value:memory_result", pointDeclaration.Id) },
            pointDeclaration.Id,
            "block:memory",
            new[]
            {
                new GuestBasicBlock(
                    "block:memory",
                    new[]
                    {
                        new GuestInstruction(
                            "array_store", null,
                            new[] { "value:memory_array", "value:memory_index", "value:memory_input" },
                            pointDeclaration.Id, null, null),
                        new GuestInstruction(
                            "array_load", "value:memory_result",
                            new[] { "value:memory_array", "value:memory_index" },
                            pointDeclaration.Id, null, null),
                    },
                    new GuestTerminator("return", null, null, null, "value:memory_result")),
            });
        module = module with
        {
            Types = layouts.Types,
            Imports = new[]
            {
                ArrayImport(
                    GuestArrayCapabilityIntrinsics.LengthImportId,
                    GuestArrayCapabilityIntrinsics.LengthImportName,
                    1),
                ArrayImport(
                    GuestArrayCapabilityIntrinsics.LoadImportId,
                    GuestArrayCapabilityIntrinsics.LoadImportName,
                    4),
                ArrayImport(
                    GuestArrayCapabilityIntrinsics.StoreImportId,
                    GuestArrayCapabilityIntrinsics.StoreImportName,
                    4),
            },
            Functions = new[] { scalar, memory },
            Exports = new[]
            {
                new GuestExport("guest_scalar_array", scalar.Id),
                new GuestExport("guest_memory_array", memory.Id),
            },
        };

        WasmCompilationResult result = WasmModuleCompiler.Compile(module);
        WasmArtifactInfo info = WasmArtifactInspector.Inspect(result.Bytes);

        Assert(result.Succeeded, "capability arrays with scalar and memory elements should compile");
        Assert(info.Imports.Select(import => import.Name).SequenceEqual(new[]
            {
                GuestArrayCapabilityIntrinsics.LengthImportName,
                GuestArrayCapabilityIntrinsics.LoadImportName,
                GuestArrayCapabilityIntrinsics.StoreImportName,
            }),
            "WASM should retain all frozen array capability imports");
        Assert(info.FunctionBodyCount == 2 && info.BoundsTrapCount == 4,
            "capability branches should retain one linear bounds trap per load/store");
    }

    private static GuestImport ArrayImport(string id, string name, int parameterCount)
    {
        return new GuestImport(
            id,
            GuestArrayCapabilityIntrinsics.Module,
            name,
            Enumerable.Repeat("type:int32", parameterCount).ToArray(),
            "type:int32");
    }

    private static void ArrayAccessEmitsBoundsTrapBeforeAddressing()
    {
        GuestModule module = WasmModuleCompilerTests.CreateMinimalModule();
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
            "function:array_load",
            new[]
            {
                new GuestRegister("value:array", arrayType.Id),
                new GuestRegister("value:index", "type:int32"),
            },
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
                            "array_load",
                            "value:result",
                            new[] { "value:array", "value:index" },
                            "type:int32",
                            null,
                            null),
                    },
                    new GuestTerminator("return", null, null, null, "value:result")),
            });
        module = module with
        {
            Types = module.Types.Concat(new[] { arrayType }).ToArray(),
            Functions = new[] { function },
            Exports = new[] { new GuestExport("guest_array_load", function.Id) },
        };

        WasmCompilationResult result = WasmModuleCompiler.Compile(module);
        WasmArtifactInfo info = WasmArtifactInspector.Inspect(result.Bytes);

        Assert(result.Succeeded, "array load should compile");
        Assert(info.BoundsTrapCount == 1,
            "array load should emit one unsigned bounds check with an explicit trap");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}

using System;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmArrayBoundsTests
{
    public static int Run()
    {
        ArrayAccessEmitsBoundsTrapBeforeAddressing();
        return 1;
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

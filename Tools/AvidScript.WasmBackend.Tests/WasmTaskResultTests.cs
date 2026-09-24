using System;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmTaskResultTests
{
    public static int Run()
    {
        GuestModule baseline = WasmModuleCompilerTests.CreateMinimalModule();
        GuestType int64 = new("type:int64", "scalar", "i64",
            Array.Empty<GuestField>(), null, null, 8, 8);
        GuestImport task = new("import:task_i32_v1", "avidscript", "avid_task_i32_v1",
            new[] { "type:int32", "type:int64", "type:int32", "type:int32" }, "type:int64");
        GuestFunction entry = new("function:task_create", Array.Empty<GuestRegister>(),
            new[]
            {
                new GuestRegister("command", "type:int32"),
                new GuestRegister("token", "type:int64"),
                new GuestRegister("argument", "type:int32"),
                new GuestRegister("reserved", "type:int32"),
                new GuestRegister("created", "type:int64"),
            }, "type:int64", "block:task_create", new[]
            {
                new GuestBasicBlock("block:task_create", new GuestInstruction[]
                {
                    new("constant", "command", Array.Empty<string>(), null, null, new("int32", "1")),
                    new("constant", "token", Array.Empty<string>(), null, null, new("int64", "0")),
                    new("constant", "argument", Array.Empty<string>(), null, null, new("int32", "0")),
                    new("constant", "reserved", Array.Empty<string>(), null, null, new("int32", "0")),
                    new("call", "created", new[] { "command", "token", "argument", "reserved" }, task.Id, null, null),
                }, new GuestTerminator("return", null, null, null, "created")),
            });
        GuestModule module = baseline with
        {
            SchemaVersion = 18,
            IrVersion = "1.17",
            Provenance = baseline.Provenance with
            {
                SemanticSchemaVersion = 35,
                SemanticVersion = "1.44",
            },
            Types = baseline.Types.Append(int64).ToArray(),
            Imports = new[] { task },
            Functions = baseline.Functions.Append(entry).ToArray(),
            Exports = baseline.Exports.Append(new GuestExport("guest_task_create", entry.Id)).ToArray(),
        };
        Require(GuestModuleValidator.Validate(module).Succeeded,
            "versioned Task<int> Guest IR must validate before codegen");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Require(compiled.Succeeded && compiled.Bytes.Length > 0,
            "Task<int> Guest call must compile to WASM");
        WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(compiled.Bytes);
        Require(artifact.Imports.Count == 1 && artifact.Imports[0].Module == "avidscript"
            && artifact.Imports[0].Name == "avid_task_i32_v1" && artifact.Imports[0].Kind == 0,
            "WASM preserves the exact versioned Host import");
        Require(artifact.Exports.Any(export => export.Name == "guest_task_create"),
            "WASM exposes the compiled task-creation call");
        Require(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes),
            "Task<int> import codegen is deterministic");
        Require(!WasmModuleCompiler.Compile(module with { SchemaVersion = 17, IrVersion = "1.16" }).Succeeded,
            "old Guest IR cannot import the Task<int> Host ABI");
        Require(!WasmModuleCompiler.Compile(module with { Imports = new[] { task with
        {
            ParameterTypeIds = new[] { "type:int64", "type:int64", "type:int32", "type:int32" },
        } } }).Succeeded, "incorrect Task<int> Host signature is rejected before codegen");
        return 7;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

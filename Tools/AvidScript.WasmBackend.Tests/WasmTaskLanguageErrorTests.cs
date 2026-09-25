using System;
using System.IO;
using System.Linq;
using System.Text;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmTaskLanguageErrorTests
{
    public static int Run()
    {
        string fixtures = Path.Combine(AppContext.BaseDirectory, "Fixtures");
        byte[] guestBytes = File.ReadAllBytes(Path.Combine(fixtures,
            "P66_TaskLanguageError.guestir.json"));
        GuestModule module = GuestIrSerializer.Deserialize(guestBytes);
        Require(module.SchemaVersion == 20 && module.IrVersion == "1.19"
            && module.Provenance.SemanticSchemaVersion == 40
            && module.Provenance.SemanticVersion == "1.49",
            "combined Task/error fixture has a distinct version identity");
        Require(GuestModuleValidator.Validate(module).Succeeded,
            "combined Task/error fixture validates before codegen");
        Require(NormalizeNewlines(guestBytes) == NormalizeNewlines(GuestIrSerializer.Serialize(module)),
            "checked-in combined fixture matches canonical Guest IR apart from line endings");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Require(compiled.Succeeded, string.Join(" | ",
            compiled.Diagnostics.Select(diagnostic => diagnostic.Message)));
        Require(compiled.Bytes.SequenceEqual(File.ReadAllBytes(Path.Combine(fixtures,
            "P66_TaskLanguageError.wasm"))),
            "checked-in WASM matches the production backend output");
        WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(compiled.Bytes);
        Require(artifact.Imports.Count == module.Imports.Count
            && artifact.Imports.Any(import => import.Module == "avidscript"
                && import.Name == "avid_task_fault_language_error_v1" && import.Kind == 0),
            "WASM retains the exact Task language-error Host import");
        Require(artifact.CustomSections.Any(section => section.Name == "avidscript.language_errors")
            && artifact.CustomSections.Any(section => section.Name == "avidscript.provenance"
                && section.PayloadText.Contains("guest_ir=20/1.19", StringComparison.Ordinal)),
            "WASM retains the versioned catalog and provenance");
        Require(!WasmModuleCompiler.Compile(module with
        {
            SchemaVersion = 19,
            IrVersion = "1.18",
        }).Succeeded, "older IR cannot compile the combined import");
        GuestFunction reader = new("function:task_language_error_read",
            new[] { new GuestRegister("task_token", "type:int64") },
            new[]
            {
                new GuestRegister("error_metadata", "type:int64"),
                new GuestRegister("error_root", "type:language_error_root"),
                new GuestRegister("one", "type:int32"),
            }, "type:int32", "entry", new[]
            {
                new GuestBasicBlock("entry", new GuestInstruction[]
                {
                    new("call", "error_metadata", new[] { "task_token" },
                        "import:task_language_error_meta_v1", null, null),
                    new("call", "error_root", new[] { "task_token" },
                        "import:task_language_error_root_v1", null, null),
                    new("constant", "one", Array.Empty<string>(), null, null,
                        new GuestConstant("int32", "1")),
                }, new GuestTerminator("return", null, null, null, "one")),
            });
        GuestModule readable = module with
        {
            Imports = module.Imports.Concat(new[]
            {
                new GuestImport("import:task_language_error_meta_v1", "avidscript",
                    "avid_task_language_error_meta_v1", new[] { "type:int64" }, "type:int64"),
                new GuestImport("import:task_language_error_root_v1", "avidscript",
                    "avid_task_language_error_root_v1", new[] { "type:int64" },
                    "type:language_error_root"),
            }).ToArray(),
            Functions = module.Functions.Append(reader).ToArray(),
        };
        Require(GuestModuleValidator.Validate(readable).Succeeded,
            "paired Task error read imports validate in IR 20");
        WasmCompilationResult readableWasm = WasmModuleCompiler.Compile(readable);
        Require(readableWasm.Succeeded, string.Join(" | ",
            readableWasm.Diagnostics.Select(diagnostic => diagnostic.Message)));
        WasmArtifactInfo readableArtifact = WasmArtifactInspector.Inspect(readableWasm.Bytes);
        Require(readableArtifact.Imports.Count == readable.Imports.Count
            && readableArtifact.Imports.Any(import => import.Module == "avidscript"
                && import.Name == "avid_task_language_error_meta_v1" && import.Kind == 0)
            && readableArtifact.Imports.Any(import => import.Module == "avidscript"
                && import.Name == "avid_task_language_error_root_v1" && import.Kind == 0),
            "production WASM retains both Task error read imports");
        return 11;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static string NormalizeNewlines(byte[] bytes) =>
        Encoding.UTF8.GetString(bytes).Replace("\r\n", "\n", StringComparison.Ordinal);
}

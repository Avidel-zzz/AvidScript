using System;
using System.IO;
using System.Linq;
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
        Require(guestBytes.SequenceEqual(GuestIrSerializer.Serialize(module)),
            "checked-in combined fixture is canonical Guest IR");
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
        return 8;
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

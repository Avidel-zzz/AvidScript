using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmCancellationTokenTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_CANCELLATION_TOKEN_WASM_DIR");
        foreach (var (name, module) in new[] {
            ("token-values", GuestCancellationTokenFixture.Values()), ("token-reader", GuestCancellationTokenFixture.WithReader()),
        }) {
            var compiled = WasmModuleCompiler.Compile(module);
            Check(compiled.Succeeded, name + ": " + string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json)).Bytes),
                name + " deterministic codegen across JSON");
            var artifact = WasmArtifactInspector.Inspect(compiled.Bytes);
            var provenance = artifact.CustomSections.Single(section => section.Name == "avidscript.provenance").PayloadText.Split('\n');
            Check(provenance.Contains("guest_ir=34/1.33") && provenance.Contains("semantic=53/1.62")
                && provenance.Contains($"guest_ir_base={module.CancellationTokens!.BaseSchemaVersion}/{module.CancellationTokens.BaseIrVersion}"),
                name + " exact source and execution metadata");
            Check(artifact.Imports.Count == module.Imports.Count, name + " no hidden imports");
            Check(new[] { "compare", "roundtrip", "none" }.All(export => artifact.Exports.Any(item => item.Name == export && item.Kind == 0)),
                name + " scalar probe exports");
            if (name == "token-reader") {
                Check(artifact.Imports.Count(import => import.Module == "avidscript"
                    && import.Name == GuestCancellationTokens.ReadImportName && import.Kind == 0) == 1, "Canonical exception reader import");
                Check(!artifact.Exports.Any(item => item.Name == "read"), "No raw managed-reference export");
                using var catalog = JsonDocument.Parse(artifact.CustomSections.Single(section => section.Name == "avidscript.language_errors").PayloadText);
                Check(catalog.RootElement.GetProperty("guest_ir_schema_version").GetInt32() == 34
                    && catalog.RootElement.GetProperty("guest_ir_version").GetString() == "1.33", "Native error catalog keeps outer contract");
            } else {
                Check(!artifact.CustomSections.Any(section => section.Name == "avidscript.language_errors"), "Pure values must not invent an error catalog");
            }
            Check(!WasmModuleCompiler.Compile(module with { CancellationTokens = null }).Succeeded, "Missing token plan reached codegen");
            Check(!WasmModuleCompiler.Compile(module with { SchemaVersion = 33, IrVersion = "1.32" }).Succeeded, "Token contract downgrade reached codegen");
            if (!string.IsNullOrWhiteSpace(directory)) {
                Directory.CreateDirectory(directory);
                File.WriteAllBytes(Path.Combine(directory, name + ".guestir.json"), json);
                File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), compiled.Bytes);
            }
        }
        return count;
    }
}

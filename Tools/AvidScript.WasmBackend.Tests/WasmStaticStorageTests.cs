using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmStaticStorageTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Static WASM: " + message);
            count++;
        }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        foreach (GuestModule module in new[] { GuestStaticStorageFixture.Create(), GuestStaticStorageFixture.WithCancellation() })
        {
            byte[] json = GuestIrSerializer.Serialize(module);
            WasmCompilationResult compiled = WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json));
            Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes), "nondeterministic static layout");
            WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(compiled.Bytes);
            string provenance = artifact.CustomSections.Single(section => section.Name == "avidscript.provenance").PayloadText;
            Check(provenance.Contains("guest_ir=27/1.26", StringComparison.Ordinal)
                && provenance.Contains($"guest_ir_base={module.StaticStorage!.BaseSchemaVersion}/{module.StaticStorage.BaseIrVersion}", StringComparison.Ordinal),
                "native loader is missing paired execution profile");
            Check(artifact.Imports.Count == module.Imports.Count, "static storage invented an undeclared import");
            Check(module.MemoryLayout.StateSlots.All(slot => module.StaticStorage!.Slots.All(value => value.Id != slot.GlobalId)),
                "static object tokens entered migratable linear state");
            var cooperative = WasmModuleCompiler.Compile(module, new(true, 4));
            Check(cooperative.Succeeded, "static storage failed cooperative codegen");
            if (module.LanguageErrorCatalog is not null)
            {
                using JsonDocument catalog = JsonDocument.Parse(artifact.CustomSections.Single(section => section.Name == "avidscript.language_errors").PayloadText);
                Check(catalog.RootElement.GetProperty("guest_ir_schema_version").GetInt32() == 27
                    && catalog.RootElement.GetProperty("guest_ir_version").GetString() == "1.26", "catalog lost outer identity");
            }
            foreach (GuestModule invalid in new[] { module with { StaticStorage = null },
                module with { SchemaVersion = module.StaticStorage!.BaseSchemaVersion, IrVersion = module.StaticStorage.BaseIrVersion },
                module with { StaticStorage = module.StaticStorage! with { BaseSchemaVersion = 27, BaseIrVersion = "1.26" } } })
            {
                var rejected = WasmModuleCompiler.Compile(invalid);
                Check(!rejected.Succeeded && rejected.Bytes.Length == 0, "malformed static IR reached WASM publication");
            }
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                string name = module.LanguageErrorCatalog is null ? "managed-static" : "managed-static-cancellation";
                File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), compiled.Bytes);
                File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
                if (module.LanguageErrorCatalog is null) File.WriteAllBytes(Path.Combine(directory, name + "-cooperative.wasm"), cooperative.Bytes);
            }
        }
        return count;
    }
}

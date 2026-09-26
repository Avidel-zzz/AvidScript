using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmTaskErrorTransferTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool condition, string message)
        {
            if (!condition) throw new InvalidOperationException("Transfer WASM: " + message);
            count++;
        }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_TASK_ERROR_TRANSFER_WASM_DIR");
        foreach (bool cached in new[] { false, true })
        {
            GuestModule module = GuestTaskErrorTransferFixture.Create(cached);
            byte[] json = GuestIrSerializer.Serialize(module);
            foreach (bool cooperative in new[] { false, true })
            {
                var options = new WasmCompilationOptions(cooperative, 4);
                var compiled = WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json), options);
                Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
                Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module, options).Bytes), "nondeterministic transfer codegen");
                var artifact = WasmArtifactInspector.Inspect(compiled.Bytes);
                string provenance = artifact.CustomSections.Single(section => section.Name == "avidscript.provenance").PayloadText;
                Check(provenance.Contains("guest_ir=28/1.27", StringComparison.Ordinal)
                    && provenance.Split('\n').Count(line => line == "guest_ir_base=24/1.23") == 1, "missing or duplicate execution profile");
                using JsonDocument catalog = JsonDocument.Parse(artifact.CustomSections.Single(section => section.Name == "avidscript.language_errors").PayloadText);
                Check(catalog.RootElement.GetProperty("guest_ir_schema_version").GetInt32() == 28
                    && catalog.RootElement.GetProperty("guest_ir_version").GetString() == "1.27", "lost outer catalog identity");
                if (!string.IsNullOrWhiteSpace(directory))
                {
                    Directory.CreateDirectory(directory);
                    string name = (cached ? "cached" : "local") + (cooperative ? "-cooperative" : "");
                    File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), compiled.Bytes);
                    File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
                }
            }
            foreach (GuestModule invalid in new[] { module with { TaskErrorTransfers = null },
                module with { SchemaVersion = 24, IrVersion = "1.23" },
                module with { TaskErrorTransfers = module.TaskErrorTransfers! with { Sites = Array.Empty<GuestTaskErrorTransferSite>() } } })
            {
                var rejected = WasmModuleCompiler.Compile(invalid);
                Check(!rejected.Succeeded && rejected.Bytes.Length == 0, "unchecked transfer reached publication");
            }
        }
        return count;
    }
}

using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmTaskCancellationErrorTests
{
    public static int Run()
    {
        GuestModule module = GuestTaskCancellationFixture.Create();
        byte[] json = GuestIrSerializer.Serialize(module);
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(json));
        Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(module).Bytes),
            "IR 24 codegen must be deterministic across canonical serialization");
        WasmArtifactInfo artifact = WasmArtifactInspector.Inspect(compiled.Bytes);
        foreach (string name in new[] { GuestTaskCancellationErrorValidator.ImportName,
            GuestTaskCancellationErrorValidator.MetaImportName, GuestTaskCancellationErrorValidator.RootImportName })
            Check(artifact.Imports.Count(import => import.Module == "avidscript"
                && import.Name == name && import.Kind == 0) == 1, "missing cancellation ABI import: " + name);
        Check(artifact.Imports.Count == module.Imports.Count,
            "cancellation compilation must not invent hidden imports");
        Check(artifact.CustomSections.Single(section => section.Name == "avidscript.provenance")
            .PayloadText.Contains("guest_ir=24/1.23", StringComparison.Ordinal), "IR 24 provenance missing");
        using JsonDocument catalog = JsonDocument.Parse(artifact.CustomSections.Single(section =>
            section.Name == "avidscript.language_errors").PayloadText);
        Check(catalog.RootElement.GetProperty("guest_ir_schema_version").GetInt32() == 24
            && catalog.RootElement.GetProperty("guest_ir_version").GetString() == "1.23"
            && catalog.RootElement.GetProperty("module_id").GetString() == module.ModuleId,
            "native loader must receive the paired version and module identity");
        Check(catalog.RootElement.GetProperty("types")[0].GetProperty("type_id").GetString()
            == GuestTaskCancellationFixture.TaskCanceledType,
            "cancellation catalog must keep the exact exception type identity");

        int count = 9;
        foreach (var (schema, version) in new[] { (20, "1.19"), (23, "1.22"), (25, "1.24") })
        {
            Check(!WasmModuleCompiler.Compile(module with
                { SchemaVersion = schema, IrVersion = version }).Succeeded,
                "unsupported cancellation IR reached production codegen");
            count++;
        }
        Check(!WasmModuleCompiler.Compile(module with
        {
            Provenance = module.Provenance with { SemanticVersion = "1.52" },
        }).Succeeded, "mismatched cancellation provenance reached codegen");
        count++;
        CheckNativeNames();
        count++;

        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_TASK_CANCELLATION_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            File.WriteAllBytes(Path.Combine(output, "task-cancellation.guestir.json"), json);
            File.WriteAllBytes(Path.Combine(output, "task-cancellation.wasm"), compiled.Bytes);
        }
        return count;
    }

    private static void CheckNativeNames()
    {
        const string relative = "Source/AvidScriptCore/Public/AvidScriptTaskResultAbi.h";
        DirectoryInfo? root = new(AppContext.BaseDirectory);
        while (root is not null && !File.Exists(Path.Combine(root.FullName, relative))) root = root.Parent;
        Check(root is not null, "native Task ABI owner not found");
        string header = File.ReadAllText(Path.Combine(root!.FullName, relative));
        foreach (var (symbol, name) in new[]
        {
            ("CancelLanguageErrorImport", GuestTaskCancellationErrorValidator.ImportName),
            ("TerminalErrorMetaImport", GuestTaskCancellationErrorValidator.MetaImportName),
            ("TerminalErrorRootImport", GuestTaskCancellationErrorValidator.RootImportName),
        }) Check(header.Contains($"{symbol}[] = \"{name}\"", StringComparison.Ordinal),
            "Guest/native cancellation import name drift: " + symbol);
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

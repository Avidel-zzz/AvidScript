using System;
using System.IO;
using System.Linq;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class WasmTaskValueCatalogTests
{
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
    public static int Run()
    {
        GuestModule plain = WasmModuleCompilerTests.CreateMinimalModule() with { ModuleId = "catalog-fixture" };
        WasmCompilationResult old = WasmModuleCompiler.Compile(plain);
        Check(old.Succeeded && !WasmArtifactInspector.Inspect(old.Bytes).CustomSections.Any(s => s.Name == GuestTaskValueCatalog.SectionName), "Absent catalog altered old artifact.");
        GuestModule module = plain with { TaskValueCatalog = GuestTaskValueCatalog.Build(plain.Types, new[] { "type:int32" }) };
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Check(compiled.Succeeded, "Catalog-bearing original Guest module failed compilation.");
        Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes), "Catalog-bearing WASM is not deterministic.");
        Check(WasmArtifactInspector.Inspect(compiled.Bytes).CustomSections.Count(s => s.Name == GuestTaskValueCatalog.SectionName) == 1, "Catalog is missing or repeated.");
        byte[] packet = module.TaskValueCatalog!.Results[0].ValuePlan.ToArray(); packet[^1] ^= 1;
        Check(!WasmModuleCompiler.Compile(module with { TaskValueCatalog = module.TaskValueCatalog with {
            Results = new[] { module.TaskValueCatalog.Results[0] with { ValuePlan = packet } } } }).Succeeded, "Emitter accepted forged task plan.");
        GuestModule managed = WasmManagedHeapTests.Create();
        string reference = managed.Types.First(type => type.Kind == "managed_ref" && type.ElementTypeId is not null).Id;
        managed = managed with { TaskValueCatalog = GuestTaskValueCatalog.Build(managed.Types, new[] { reference }) };
        WasmCompilationResult managedResult = WasmModuleCompiler.Compile(managed);
        Check(managedResult.Succeeded && WasmArtifactInspector.Inspect(managedResult.Bytes).CustomSections.Any(s => s.Name == GuestTaskValueCatalog.SectionName),
            "Actual managed heap module failed with its shared-ordinal catalog.");
        Check(GuestManagedHeap.TypeOrdinals(managed.Types).Where(entry => entry.Value != 0).Select(entry => entry.Value)
            .SequenceEqual(Enumerable.Range(1, managed.Types.Count(type => type.Kind == "managed_ref" && type.ElementTypeId is not null))),
            "Shared nominal heap ordinals are not consecutive.");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_TASK_VALUE_CATALOG_DIR");
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory); File.WriteAllBytes(Path.Combine(directory, "scalar.wasm"), compiled.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "absent.wasm"), old.Bytes);
            File.WriteAllBytes(Path.Combine(directory, "scalar-wasm.catalog"), GuestTaskValueCatalog.Encode(module));
            Console.WriteLine("AvidScript.TaskValueCatalog.WasmFixtures: 2/2 written");
        }
        return 4;
    }
}
